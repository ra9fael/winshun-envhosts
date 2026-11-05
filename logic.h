// logic.h - pure, bounded computation for the env / hosts WinShun plugin.
//
// This header has no OS dependencies, performs no I/O, holds no global mutable
// state, and never throws across its public API. Every entry point is a
// deterministic function of its arguments. main.cpp is the only file that talks
// to the host SDK and the Win32 registry / file system; native.h holds the OS
// calls. Keeping the logic separate makes it possible to test the parsers and
// the byte-exact hosts math without starting a plugin process or touching the
// registry.
//
// Code and comments are English; user-visible strings are produced in main.cpp
// with the host locale.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace envh {

// ---- bounds ----------------------------------------------------------------
constexpr std::size_t kMaxInputChars = 16 * 1024; // host caps a query line here
constexpr std::size_t kMaxEnvName = 255;
constexpr std::size_t kMaxEnvValue = 32767; // Windows process environment block limit
constexpr std::size_t kEnvBlockWarn = 30000; // conservative guard before the limit
constexpr std::size_t kMaxHostsLines = 200 * 1024;
constexpr std::size_t kMaxRows = 200;

// ---- failure ----------------------------------------------------------------
struct Failure {
    std::string code;
    std::string message;
    std::string hint;
};

template <typename T>
struct Outcome {
    bool success = false;
    T value{};
    Failure failure;
};

inline Failure fail(const std::string& code, const std::string& message, const std::string& hint)
{
    return Failure {code, message, hint};
}

// ---- small text helpers ------------------------------------------------------
inline std::string toLower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

inline std::string trimCopy(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

inline bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Stable 64-bit FNV-1a, rendered as 16 hex digits (used for host entry ids).
inline std::string fnv1aHex(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

// ---- hosts model -------------------------------------------------------------
// The file is kept as raw lines so untouched lines survive byte-for-byte
// (BOM, CRLF/LF mix, trailing newline, comments, blank lines all preserved).
struct HostsLine {
    std::string text; // the line without its terminator
    bool crlf = false; // true: "\r\n", false: "\n"
};

struct HostsParsed {
    std::string bom; // the leading UTF-8 BOM ("\xEF\xBB\xBF") or ""
    std::vector<HostsLine> lines;
    bool trailingNewline = true;
};

struct HostsEntry {
    int lineIndex = -1; // index into HostsParsed::lines
    bool commented = false; // leading '#' present
    bool section = false; // comment-only divider (no ip/host spec)
    std::string ip;
    std::string hostname;
    std::string comment; // trailing comment after the host spec
    std::string raw; // the raw line text (text + terminator already stripped)
};

// Split raw content into bom + lines + trailing-newline flag.
inline HostsParsed parseHosts(const std::string& content)
{
    HostsParsed h;
    std::size_t at = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF
        && static_cast<unsigned char>(content[1]) == 0xBB
        && static_cast<unsigned char>(content[2]) == 0xBF) {
        h.bom = content.substr(0, 3);
        at = 3;
    }
    const std::string body = content.substr(at);
    if (body.empty()) {
        h.trailingNewline = false;
        return h;
    }
    // Split on '\n'. Every segment keeps its own (possible) trailing '\r'.
    std::size_t start = 0;
    std::vector<std::string> segs;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\n') {
            segs.push_back(body.substr(start, i - start));
            start = i + 1;
        }
    }
    // The remainder after the last '\n' (possibly empty) is the final segment;
    // its presence means the file ended with a newline.
    segs.push_back(body.substr(start));
    if (segs.empty()) {
        h.trailingNewline = false;
        return h;
    }
    // If the original ended with '\n', the final segment is empty -> drop it and
    // remember the trailing newline.
    if (segs.back().empty()) {
        segs.pop_back();
        h.trailingNewline = true;
    } else {
        h.trailingNewline = false;
    }
    for (std::string& seg : segs) {
        HostsLine line;
        if (!seg.empty() && seg.back() == '\r') {
            seg.pop_back();
            line.crlf = true;
        }
        line.text = seg;
        h.lines.push_back(std::move(line));
    }
    return h;
}

// Serialize back to bytes. Untouched lines are byte-identical to the source.
inline std::string serializeHosts(const HostsParsed& h)
{
    std::string out = h.bom;
    const std::string term = "\n";
    for (std::size_t i = 0; i < h.lines.size(); ++i) {
        out += h.lines[i].text;
        out += h.lines[i].crlf ? "\r\n" : term;
    }
    if (!h.trailingNewline && !out.empty()) {
        // The source had no final newline; remove the terminator we appended
        // after the last line.
        if (out.size() >= 2 && out.substr(out.size() - 2) == "\r\n")
            out.resize(out.size() - 2);
        else if (!out.empty() && out.back() == '\n')
            out.pop_back();
    }
    return out;
}

inline bool lineIsCommented(const std::string& raw)
{
    std::size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
        ++i;
    return i < raw.size() && raw[i] == '#';
}

// Prepend/remove a leading "# " on a raw line (terminator already stripped).
inline std::string toggleCommentOnLine(const std::string& raw, bool wantCommented)
{
    std::size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
        ++i;
    const bool isComment = (i < raw.size() && raw[i] == '#');
    if (wantCommented == isComment)
        return raw;
    if (wantCommented)
        return raw.substr(0, i) + "# " + raw.substr(i);
    std::size_t j = i + 1;
    if (j < raw.size() && raw[j] == ' ')
        ++j;
    return raw.substr(0, i) + raw.substr(j);
}

// Parse one raw line into a host spec (after stripping any comment marker).
inline bool ipv4Valid(const std::string& s); // defined below; forward-declared for parseHostLine
inline HostsEntry parseHostLine(const std::string& raw, int lineIndex)
{
    HostsEntry e;
    e.lineIndex = lineIndex;
    e.raw = raw;
    e.commented = lineIsCommented(raw);
    std::string body = raw;
    if (e.commented) {
        std::size_t i = 0;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
            ++i;
        if (i < body.size() && body[i] == '#') {
            ++i;
            if (i < body.size() && body[i] == ' ')
                ++i;
            body = body.substr(i);
        }
    }
    body = trimCopy(body);
    if (body.empty()) {
        e.section = e.commented; // a pure "#" line is a divider
        return e;
    }
    // Tokenize on whitespace.
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
            ++i;
        if (i >= body.size())
            break;
        std::size_t j = i;
        while (j < body.size() && body[j] != ' ' && body[j] != '\t')
            ++j;
        tok.push_back(body.substr(i, j - i));
        i = j;
    }
    // A trailing comment begins with '#'.
    if (!tok.empty() && tok.back().size() > 1 && tok.back()[0] == '#') {
        e.comment = tok.back().substr(1);
        tok.pop_back();
    }
    bool hostSpec = false;
    if (!tok.empty()) {
        const bool ipv4 = ipv4Valid(tok[0]);
        const bool ipv6 = tok[0].find(':') != std::string::npos;
        hostSpec = (tok.size() >= 2) && (ipv4 || ipv6);
    }
    // A commented line with no host spec (e.g. "# Added by Docker Desktop") is a
    // section divider, not a disabled entry.
    e.section = e.commented && !hostSpec;
    if (!e.section && !tok.empty()) {
        e.ip = tok[0];
        if (tok.size() > 1)
            e.hostname = tok[1];
    }
    return e;
}

inline std::vector<HostsEntry> hostsEntries(const HostsParsed& h)
{
    std::vector<HostsEntry> out;
    for (std::size_t i = 0; i < h.lines.size(); ++i) {
        const std::string& raw = h.lines[i].text;
        if (raw.empty())
            continue; // skip blank lines; they round-trip via the line list
        out.push_back(parseHostLine(raw, static_cast<int>(i)));
    }
    return out;
}

// A normalized key for id hashing: stable across line reordering and across the
// current comment state.
inline std::string hostsNormKey(const HostsEntry& e)
{
    return std::string(e.commented ? "C" : "E") + "|" + (e.section ? "S" : "") + "|" + e.ip + "|"
           + toLower(e.hostname) + "|" + e.comment;
}

inline std::string hostsEntryId(const HostsEntry& e)
{
    return "hosts:" + fnv1aHex(hostsNormKey(e));
}

// ---- hosts transforms (pure; the OS write happens in native.h) ---------------

inline Outcome<HostsParsed> hostsToggle(const HostsParsed& in, int lineIndex)
{
    HostsParsed h = in;
    if (lineIndex < 0 || static_cast<std::size_t>(lineIndex) >= h.lines.size())
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_LINE", "line out of range", "hosts docker")};
    const HostsEntry e = parseHostLine(h.lines[lineIndex].text, lineIndex);
    if (e.section)
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_SECTION", "section comment cannot be toggled", "hosts docker")};
    const bool wantCommented = !e.commented;
    h.lines[lineIndex].text = toggleCommentOnLine(h.lines[lineIndex].text, wantCommented);
    return Outcome<HostsParsed> {true, std::move(h), {}};
}

inline Outcome<HostsParsed> hostsDelete(const HostsParsed& in, int lineIndex)
{
    HostsParsed h = in;
    if (lineIndex < 0 || static_cast<std::size_t>(lineIndex) >= h.lines.size())
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_LINE", "line out of range", "hosts docker")};
    h.lines.erase(h.lines.begin() + lineIndex);
    return Outcome<HostsParsed> {true, std::move(h), {}};
}

inline bool ipv4Valid(const std::string& s)
{
    if (s.empty())
        return false;
    std::size_t start = 0;
    int octets = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (i == start)
                return false;
            const std::string oct = s.substr(start, i - start);
            if (oct.size() > 3)
                return false;
            int v = 0;
            for (char c : oct) {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10 + (c - '0');
            }
            if (v > 255)
                return false;
            ++octets;
            start = i + 1;
        } else if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return octets == 4;
}

inline bool hostValid(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '#')
            return false;
    return true;
}

inline Outcome<HostsParsed> hostsAdd(const HostsParsed& in, const std::string& ip,
                                     const std::string& hostname, const std::string& comment)
{
    const std::string ipT = trimCopy(ip);
    const std::string hostT = trimCopy(hostname);
    const std::string cmtT = trimCopy(comment);
    if (ipT.empty() || hostT.empty())
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_FMT", "need an ip and a hostname", "hosts add 127.0.0.1 example.test")};
    const bool isIpv4 = ipv4Valid(ipT);
    const bool isIpv6 = !isIpv4 && ipT.find(':') != std::string::npos;
    if (!isIpv4 && !isIpv6)
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_IP", "not a valid IPv4 or IPv6 address", "hosts add 127.0.0.1 example.test")};
    if (!hostValid(hostT))
        return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_HOST", "hostname has spaces or a '#'", "hosts add 127.0.0.1 example.test")};
    // Reject a duplicate of an enabled entry.
    const std::vector<HostsEntry> existing = hostsEntries(in);
    const std::string lh = toLower(hostT);
    for (const HostsEntry& e : existing) {
        if (!e.commented && !e.section && toLower(e.hostname) == lh)
            return Outcome<HostsParsed> {false, {}, fail("E_HOSTS_DUP",
                "a live entry already maps this hostname; toggle it instead", "hosts " + hostT)};
    }
    HostsParsed h = in;
    // Append after the last content line, reusing the file's dominant ending.
    bool crlf = false;
    for (const HostsLine& l : h.lines)
        if (!l.text.empty())
            crlf = l.crlf;
    std::string line = ipT + " " + hostT;
    if (!cmtT.empty())
        line += " # " + cmtT;
    HostsLine nl;
    nl.text = line;
    nl.crlf = crlf;
    // Ensure a blank separator exists before appending, if the file is non-empty.
    if (!h.lines.empty() && !h.lines.back().text.empty())
        h.lines.push_back(HostsLine {}); // blank line
    h.lines.push_back(nl);
    if (!h.trailingNewline)
        h.trailingNewline = true;
    return Outcome<HostsParsed> {true, std::move(h), {}};
}

// Locate an entry by its stable id among the current parse (robust to external
// edits between list and action).
inline bool findHostsEntryById(const HostsParsed& h, const std::string& id, int& outIndex)
{
    const std::vector<HostsEntry> entries = hostsEntries(h);
    for (const HostsEntry& e : entries) {
        if (hostsEntryId(e) == id) {
            outIndex = e.lineIndex;
            return true;
        }
    }
    return false;
}

// ---- env model ---------------------------------------------------------------
struct EnvVar {
    std::string scope; // "user" | "system"
    std::string name; // original case
    std::string type; // "REG_SZ" | "REG_EXPAND_SZ" | "REG_MULTI_SZ"
    std::string value;
};

struct EnvSnapshot {
    std::vector<EnvVar> vars; // user-scope first, then system-scope
};

inline std::string envId(const EnvVar& v)
{
    return "env:" + v.scope + ":" + toLower(v.name);
}

inline EnvVar* findEnvVar(EnvSnapshot& s, const std::string& name, const std::string& scope)
{
    const std::string ln = toLower(name);
    for (EnvVar& v : s.vars)
        if (v.scope == scope && toLower(v.name) == ln)
            return &v;
    return nullptr;
}

inline const EnvVar* findEnvVar(const EnvSnapshot& s, const std::string& name, const std::string& scope)
{
    const std::string ln = toLower(name);
    for (const EnvVar& v : s.vars)
        if (v.scope == scope && toLower(v.name) == ln)
            return &v;
    return nullptr;
}

inline bool isValidEnvName(const std::string& name)
{
    if (name.empty() || name.size() > kMaxEnvName)
        return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F)
            return false;
        if (c == '=' || c == ' ' || c == '\t')
            return false;
    }
    return true;
}

// A value may contain anything except control characters (NUL cannot even be
// carried by JSON, so it is rejected here). The 32767 cap is the OS block limit.
inline bool isValidEnvValue(const std::string& value)
{
    if (value.size() > kMaxEnvValue)
        return false;
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

inline std::string envTypeForValue(const std::string& value)
{
    // REG_EXPAND_SZ only when the value references another variable.
    return value.find('%') != std::string::npos ? "REG_EXPAND_SZ" : "REG_SZ";
}

// Replace %NAME% occurrences using a lowercase-keyed lookup; unresolved names are
// left verbatim (matching Windows behaviour for unset variables).
inline std::string expandEnvValue(const std::string& value, const std::map<std::string, std::string>& kv)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] == '%') {
            std::size_t j = i + 1;
            while (j < value.size() && value[j] != '%')
                ++j;
            if (j < value.size()) {
                const std::string name = value.substr(i + 1, j - i - 1);
                const auto it = kv.find(toLower(name));
                if (it != kv.end())
                    out += it->second;
                else
                    out += value.substr(i, j - i + 1);
                i = j + 1;
            } else {
                out += value[i];
                ++i;
            }
        } else {
            out += value[i];
            ++i;
        }
    }
    return out;
}

// A lowercase-keyed lookup over both scopes (user overrides system by insertion
// order is not guaranteed; callers build it user-first).
inline std::map<std::string, std::string> envLookup(const EnvSnapshot& s)
{
    std::map<std::string, std::string> kv;
    for (const EnvVar& v : s.vars)
        kv[toLower(v.name)] = v.value; // later entries overwrite earlier
    return kv;
}

inline Outcome<EnvSnapshot> envSet(EnvSnapshot snap, const std::string& name,
                                   const std::string& value, const std::string& scope)
{
    const std::string n = trimCopy(name);
    if (!isValidEnvName(n))
        return Outcome<EnvSnapshot> {false, {}, fail("E_NAME", "name needs 1-255 chars, no '=' or control chars",
                                                    "env:set MY_VAR=value")};
    if (!isValidEnvValue(value))
        return Outcome<EnvSnapshot> {false, {}, fail("E_VALUE", "value too long or has control chars",
                                                     "env:set MY_VAR=a short value")};
    const std::string type = envTypeForValue(value);
    EnvVar* existing = findEnvVar(snap, n, scope);
    if (existing) {
        existing->value = value;
        existing->type = type;
    } else {
        EnvVar v;
        v.scope = scope;
        v.name = n;
        v.type = type;
        v.value = value;
        snap.vars.push_back(std::move(v));
    }
    return Outcome<EnvSnapshot> {true, std::move(snap), {}};
}

inline Outcome<EnvSnapshot> envDelete(EnvSnapshot snap, const std::string& name, const std::string& scope)
{
    const std::string n = trimCopy(name);
    if (!isValidEnvName(n))
        return Outcome<EnvSnapshot> {false, {}, fail("E_NAME", "invalid variable name", "env path")};
    EnvVar* existing = findEnvVar(snap, n, scope);
    if (!existing)
        return Outcome<EnvSnapshot> {false, {}, fail("E_NOTFOUND", "variable not present in this scope",
                                                     "env " + n)};
    for (auto it = snap.vars.begin(); it != snap.vars.end(); ++it) {
        if (&*it == existing) {
            snap.vars.erase(it);
            break;
        }
    }
    return Outcome<EnvSnapshot> {true, std::move(snap), {}};
}

// Names that deserve an extra warning before deletion.
inline bool isWellKnownName(const std::string& name)
{
    static const char* kNames[] = {"PATH", "TEMP", "TMP", "TMPDIR", "SystemRoot", "SystemDrive",
                                   "USERPROFILE", "USERNAME", "COMSPEC", "WINDIR", "OS"};
    const std::string ln = toLower(name);
    for (const char* n : kNames)
        if (ln == toLower(std::string(n)))
            return true;
    return false;
}

// ---- grammar (stateless; every state lives in the query text) ----------------
enum class CmdKind {
    EnvList,
    EnvSet,
    EnvNew,
    EnvRestore,
    HostsList,
    HostsAdd,
    HostsRestore,
    Unknown
};

struct ParsedQuery {
    CmdKind kind = CmdKind::Unknown;
    std::string filter; // EnvList / HostsList
    std::string name; // EnvSet / EnvNew
    std::string value; // EnvSet
    std::string ip; // HostsAdd
    std::string hostname; // HostsAdd
    std::string comment; // HostsAdd
    bool hasError = false;
    std::string errorCode;
    std::string errorHint;
};

inline ParsedQuery parseQuery(const std::string& command, const std::string& text)
{
    ParsedQuery q;
    const std::string t = trimCopy(text);
    if (command == "env") {
        if (t.empty())
            q.kind = CmdKind::EnvList;
        else if (t == "new")
            q.kind = CmdKind::EnvNew;
        else if (t == "restore")
            q.kind = CmdKind::EnvRestore;
        else if (startsWith(t, "set ")) {
            const std::string rest = t.substr(4);
            const std::size_t eq = rest.find('=');
            if (eq == std::string::npos) {
                q.hasError = true;
                q.errorCode = "E_ENV_SET";
                q.errorHint = "env:set NAME=value";
            } else {
                q.kind = CmdKind::EnvSet;
                q.name = trimCopy(rest.substr(0, eq));
                q.value = rest.substr(eq + 1);
            }
        } else {
            q.kind = CmdKind::EnvList;
            q.filter = t;
        }
    } else if (command == "hosts") {
        if (t.empty())
            q.kind = CmdKind::HostsList;
        else if (t == "restore")
            q.kind = CmdKind::HostsRestore;
        else if (startsWith(t, "add ")) {
            const std::string rest = trimCopy(t.substr(4));
            // ip hostname [# comment]
            std::size_t a = 0;
            while (a < rest.size() && (rest[a] == ' ' || rest[a] == '\t'))
                ++a;
            std::size_t b = a;
            while (b < rest.size() && rest[b] != ' ' && rest[b] != '\t')
                ++b;
            const std::string ip = rest.substr(a, b - a);
            a = b;
            while (a < rest.size() && (rest[a] == ' ' || rest[a] == '\t'))
                ++a;
            b = a;
            while (b < rest.size() && rest[b] != ' ' && rest[b] != '\t')
                ++b;
            const std::string host = rest.substr(a, b - a);
            std::string comment = trimCopy(rest.substr(b));
            if (startsWith(comment, "#"))
                comment = trimCopy(comment.substr(1));
            if (ip.empty() || host.empty()) {
                q.hasError = true;
                q.errorCode = "E_HOSTS_ADD";
                q.errorHint = "hosts add 127.0.0.1 example.test";
            } else {
                q.kind = CmdKind::HostsAdd;
                q.ip = ip;
                q.hostname = host;
                q.comment = comment;
            }
        } else {
            q.kind = CmdKind::HostsList;
            q.filter = t;
        }
    } else {
        q.hasError = true;
        q.errorCode = "E_COMMAND";
        q.errorHint = "env | hosts";
    }
    return q;
}

// ---- row helpers used by main.cpp --------------------------------------------

// Filter an env var by a case-insensitive substring on name or value.
inline bool envMatches(const EnvVar& v, const std::string& filter)
{
    if (filter.empty())
        return true;
    const std::string lf = toLower(filter);
    return toLower(v.name).find(lf) != std::string::npos
           || toLower(v.value).find(lf) != std::string::npos;
}

// Filter a hosts entry by substring on ip / hostname / comment.
inline bool hostsMatches(const HostsEntry& e, const std::string& filter)
{
    if (filter.empty())
        return true;
    const std::string lf = toLower(filter);
    return toLower(e.ip).find(lf) != std::string::npos
           || toLower(e.hostname).find(lf) != std::string::npos
           || toLower(e.comment).find(lf) != std::string::npos;
}

// Estimate the user-scope environment block size (sum of "name=value\0").
inline std::size_t envBlockSize(const EnvSnapshot& s)
{
    std::size_t total = 0;
    for (const EnvVar& v : s.vars)
        if (v.scope == "user")
            total += v.name.size() + 1 + v.value.size() + 1 + 1;
    return total;
}

} // namespace envh
