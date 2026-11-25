// ssh_model.h - SSH client config and key model.
//
// Pure logic, no OS calls, no Qt: everything here is unit-testable on any C++17
// compiler (tests/test_ssh_model.cpp). The Windows layer (native.h: SshStore,
// directory listing, ssh-keygen) and the Qt bridge only move bytes and values
// through this model, so the rules below are the single definition of what a
// save means.
//
// Two invariants drive the design:
//
//  1. Byte-exact preservation. Lines this editor did not touch are written back
//     exactly as they were read: comments, blank lines, unknown directives,
//     directive order and the trailing newline all survive. Only a block whose
//     fields were edited is regenerated, and even then its unknown directives
//     are kept in their original order.
//  2. Honest read-only ranges. `Match`, `Include`, `Host` with a wildcard and
//     free-standing preamble lines cannot be expressed in the field form, so
//     they are marked read-only and always written back verbatim. Editing them
//     would silently rewrite something the form cannot represent.
//
// Code and comments are English.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace envh {
namespace ssh {

// ---- small text helpers -------------------------------------------------------

inline std::string toLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trim(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

// Fuzzy subsequence match with bonuses for word boundaries and adjacent chars.
inline int fuzzyScore(const std::string& text, const std::string& query)
{
    const std::string hay = toLower(text);
    const std::string needle = toLower(query);
    if (needle.empty())
        return 0;
    std::size_t pos = 0;
    int score = 0;
    int previous = -2;
    for (char wanted : needle) {
        const std::size_t found = hay.find(wanted, pos);
        if (found == std::string::npos)
            return -1000000;
        const bool boundary = found == 0 || !(std::isalnum(static_cast<unsigned char>(hay[found - 1])) || hay[found - 1] == '_');
        score += 10 + (boundary ? 12 : 0) + (static_cast<int>(found) == previous + 1 ? 8 : 0) - static_cast<int>(found - pos);
        previous = static_cast<int>(found);
        pos = found + 1;
    }
    return score - static_cast<int>(hay.size() - needle.size());
}

inline bool startsWith(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline bool endsWith(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

// ---- SHA-256 (only used for key fingerprints) ---------------------------------

namespace sha_detail {

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline std::uint32_t load32be(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
           | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void store32be(unsigned char* p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char>(v & 0xFF);
}

} // namespace sha_detail

// Writes 32 raw bytes into `out`. FIPS 180-4; the key blobs are tiny, so a
// straightforward one-shot implementation is plenty.
inline void sha256(const std::string& data, unsigned char out[32])
{
    using sha_detail::load32be;
    using sha_detail::rotr;
    using sha_detail::store32be;

    static const std::uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    std::string msg = data;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56)
        msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xFF));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data() + off);
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = load32be(p + i * 4);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    for (int i = 0; i < 8; ++i)
        store32be(out + i * 4, h[i]);
}

// ---- base64 -------------------------------------------------------------------

inline std::string base64(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const std::uint32_t v = (static_cast<unsigned char>(in[i]) << 16)
                                | (static_cast<unsigned char>(in[i + 1]) << 8)
                                | static_cast<unsigned char>(in[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    const std::size_t rest = in.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<unsigned char>(in[i]) << 16)
                                | (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

inline std::string base64TrimPad(std::string s)
{
    while (!s.empty() && s.back() == '=')
        s.pop_back();
    return s;
}

// Returns the decoded bytes, or an empty string when the input is not base64
// (the caller distinguishes "empty" from "invalid" by checking the input).
inline std::string base64Decode(const std::string& in)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        const int v = value(c);
        if (v < 0)
            return {};
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// OpenSSH format: "SHA256:" + unpadded base64 of the SHA-256 of the key blob.
inline std::string fingerprintOfBlob(const std::string& blob)
{
    unsigned char digest[32];
    sha256(blob, digest);
    return "SHA256:" + base64TrimPad(base64(std::string(reinterpret_cast<char*>(digest), 32)));
}

// ---- SSH wire format (RFC 4251 mpints / strings) ------------------------------

// Reads one length-prefixed field; advances `off`. False when truncated.
inline bool readField(const std::string& blob, std::size_t& off, std::string& out)
{
    if (off + 4 > blob.size())
        return false;
    const std::size_t n = (static_cast<unsigned char>(blob[off]) << 24)
                          | (static_cast<unsigned char>(blob[off + 1]) << 16)
                          | (static_cast<unsigned char>(blob[off + 2]) << 8)
                          | static_cast<unsigned char>(blob[off + 3]);
    off += 4;
    if (off + n > blob.size())
        return false;
    out = blob.substr(off, n);
    off += n;
    return true;
}

// Bit length of an unsigned big-endian integer (the RSA modulus).
inline int bitLength(const std::string& mpint)
{
    std::size_t i = 0;
    while (i < mpint.size() && mpint[i] == 0)
        ++i;
    if (i >= mpint.size())
        return 0;
    int bits = static_cast<int>((mpint.size() - i) * 8);
    unsigned char top = static_cast<unsigned char>(mpint[i]);
    for (int b = 7; b >= 0; --b) {
        if (top & (1u << b))
            break;
        --bits;
    }
    return bits;
}

// ---- public key ---------------------------------------------------------------

struct PublicKey {
    bool ok = false;
    std::string algo; // as written in the file ("ssh-ed25519", "ssh-rsa", ...)
    std::string blob; // decoded wire blob
    std::string comment;
    std::string type; // "ed25519" | "rsa" | "ecdsa" | "dss" | "" (unknown)
    int bits = 0;
    std::string fingerprint; // "SHA256:..." (empty when the blob is unreadable)
    std::string error; // "empty" | "format" | "base64" | "blob"
};

inline std::string typeOfAlgo(const std::string& algo)
{
    const std::string a = toLower(algo);
    if (startsWith(a, "ssh-ed25519") || startsWith(a, "sk-ssh-ed25519"))
        return "ed25519";
    if (startsWith(a, "ssh-rsa"))
        return "rsa";
    if (startsWith(a, "ecdsa-sha2-"))
        return "ecdsa";
    if (startsWith(a, "ssh-dss"))
        return "dss";
    return {};
}

// Parses the single line of a .pub file: "<algo> <base64> [comment]".
inline PublicKey parsePublicKey(const std::string& text)
{
    PublicKey k;
    // A .pub file may carry one entry per line; the first non-empty line is the
    // key. Blank and comment lines are skipped.
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#')
            continue;

        const std::size_t s1 = line.find_first_of(" \t");
        if (s1 == std::string::npos) {
            k.error = "format";
            return k;
        }
        const std::size_t s2 = line.find_first_not_of(" \t", s1);
        if (s2 == std::string::npos) {
            k.error = "format";
            return k;
        }
        std::size_t s3 = line.find_first_of(" \t", s2);
        const std::string b64 = line.substr(s2, s3 == std::string::npos ? std::string::npos : s3 - s2);
        k.algo = line.substr(0, s1);
        if (s3 != std::string::npos) {
            const std::size_t c = line.find_first_not_of(" \t", s3);
            if (c != std::string::npos)
                k.comment = line.substr(c);
        }
        k.blob = base64Decode(b64);
        if (k.blob.empty()) {
            k.error = "base64";
            return k;
        }
        k.type = typeOfAlgo(k.algo);
        k.fingerprint = fingerprintOfBlob(k.blob);

        // Bits: fixed for ed25519/ecdsa, from the modulus for rsa.
        if (k.type == "ed25519") {
            k.bits = 256;
        } else if (k.type == "ecdsa") {
            if (endsWith(toLower(k.algo), "nistp256"))
                k.bits = 256;
            else if (endsWith(toLower(k.algo), "nistp384"))
                k.bits = 384;
            else if (endsWith(toLower(k.algo), "nistp521"))
                k.bits = 521;
        } else if (k.type == "rsa") {
            std::size_t off = 0;
            std::string algo, e, n;
            if (readField(k.blob, off, algo) && readField(k.blob, off, e) && readField(k.blob, off, n))
                k.bits = bitLength(n);
        } else if (k.type == "dss") {
            k.bits = 1024;
        }
        if (k.algo.empty() || k.type.empty()) {
            k.error = "blob";
            return k;
        }
        k.ok = true;
        return k;
    }
    k.error = "empty";
    return k;
}

// ---- key files ----------------------------------------------------------------

enum class KeyState { Both, PublicOnly, PrivateOnly };

struct KeyFile {
    std::string name; // file name inside the SSH directory
    KeyState state = KeyState::Both;
    PublicKey pub;
    // A private key we found but could not pair with a readable .pub file.
    bool privateUnreadable = false;
};

struct KnownHost {
    int line = -1;
    std::string hosts;
    std::string port;
    std::string type;
    std::string fingerprint;
    std::string raw;
};

inline std::vector<KnownHost> parseKnownHosts(const std::string& text)
{
    std::vector<KnownHost> out;
    std::size_t pos = 0;
    int line = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string raw = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!raw.empty() && raw.front() != '#') {
            std::vector<std::string> fields;
            std::size_t b = 0;
            while (b < raw.size()) {
                while (b < raw.size() && std::isspace(static_cast<unsigned char>(raw[b])))
                    ++b;
                if (b == raw.size())
                    break;
                std::size_t e = b;
                while (e < raw.size() && !std::isspace(static_cast<unsigned char>(raw[e])))
                    ++e;
                fields.push_back(raw.substr(b, e - b));
                b = e;
            }
            const std::size_t marker = !fields.empty() && fields[0].size() > 1 && fields[0][0] == '@' ? 1 : 0;
            if (fields.size() >= marker + 3
                && (fields[marker + 1].find("ssh-") == 0 || fields[marker + 1].find("ecdsa-") == 0
                    || fields[marker + 1].find("sk-") == 0)) {
                PublicKey key = parsePublicKey(fields[marker + 1] + " " + fields[marker + 2]);
                if (key.ok) {
                    KnownHost h;
                    h.line = line;
                    h.hosts = fields[marker];
                    h.type = key.type;
                    h.fingerprint = key.fingerprint;
                    const std::size_t close = h.hosts.find(']');
                    if (h.hosts.size() > 3 && h.hosts.front() == '[' && close != std::string::npos
                        && close + 1 < h.hosts.size() && h.hosts[close + 1] == ':')
                        h.port = h.hosts.substr(close + 2);
                    h.raw = raw;
                    out.push_back(std::move(h));
                }
            }
        }
        ++line;
    }
    return out;
}

inline KeyFile makeKeyFile(const std::string& name, bool hasPrivate, bool hasPublic, const std::string& pubText)
{
    KeyFile f;
    f.name = name;
    f.state = hasPublic ? (hasPrivate ? KeyState::Both : KeyState::PublicOnly) : KeyState::PrivateOnly;
    if (hasPublic)
        f.pub = parsePublicKey(pubText);
    f.privateUnreadable = hasPrivate && !hasPublic;
    return f;
}

inline const char* keyStateName(KeyState s)
{
    switch (s) {
    case KeyState::Both:
        return "both";
    case KeyState::PublicOnly:
        return "public";
    case KeyState::PrivateOnly:
        return "private";
    }
    return "both";
}

// A key file name is a bare name inside the SSH directory: no separators, no
// wildcards, no reserved characters. Dots are fine ("id_ed25519", "my.key").
inline bool validKeyFileName(const std::string& name)
{
    if (name.empty() || name.size() > 100)
        return false;
    if (name == "." || name == "..")
        return false;
    if (name.front() == ' ' || name.back() == ' ')
        return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F)
            return false;
        if (std::strchr("\\/:*?\"<>|", c) != nullptr)
            return false;
    }
    return true;
}

// ---- ssh-keygen ---------------------------------------------------------------

struct KeygenSpec {
    std::string type = "ed25519"; // UI token: ed25519 | rsa2048 | rsa4096 | ecdsa256
    std::string fileName;
    std::string comment;
    std::string passphrase; // empty = no passphrase
};

struct Check {
    bool ok = true;
    std::string code; // "E_KEYNAME" | "E_KEYEXISTS" | "E_KEYTYPE"
    std::string detail;
};

// `existing` holds the file names already present in the SSH directory.
inline Check validateKeygen(const KeygenSpec& spec, const std::vector<std::string>& existing)
{
    Check c;
    const std::string type = toLower(spec.type);
    if (type != "ed25519" && type != "rsa2048" && type != "rsa4096" && type != "ecdsa256") {
        c.ok = false;
        c.code = "E_KEYTYPE";
        return c;
    }
    if (!validKeyFileName(spec.fileName)) {
        c.ok = false;
        c.code = "E_KEYNAME";
        return c;
    }
    for (const auto& n : existing) {
        // Windows file names are case-insensitive, and ssh-keygen refuses a key
        // whose path differs only by case.
        const std::string a = toLower(spec.fileName), b = toLower(n);
        if (b == a || b == a + ".pub") {
            c.ok = false;
            c.code = "E_KEYEXISTS";
            c.detail = n;
            return c;
        }
    }
    return c;
}

// ssh-keygen arguments for a key at `keyPath` (no directory listing, no
// overwrite: -f refuses to clobber an existing file, and validateKeygen has
// already checked).
inline std::vector<std::string> keygenArgs(const KeygenSpec& spec, const std::string& keyPath)
{
    const std::string type = toLower(spec.type);
    std::vector<std::string> a {"-q", "-t"};
    if (type == "rsa2048") {
        a.push_back("rsa");
        a.push_back("-b");
        a.push_back("2048");
    } else if (type == "rsa4096") {
        a.push_back("rsa");
        a.push_back("-b");
        a.push_back("4096");
    } else if (type == "ecdsa256") {
        a.push_back("ecdsa");
        a.push_back("-b");
        a.push_back("256");
    } else {
        a.push_back("ed25519");
    }
    a.push_back("-f");
    a.push_back(keyPath);
    a.push_back("-N");
    a.push_back(spec.passphrase);
    if (!spec.comment.empty()) {
        a.push_back("-C");
        a.push_back(spec.comment);
    }
    return a;
}

// ---- config model -------------------------------------------------------------

enum class BlockType { Host, Match, Include, Preamble };

struct Directive {
    std::string key; // lower-case keyword
    std::string value;
    std::string raw; // the line exactly as read
};

// The editable fields of a Host block.
struct Conn {
    std::string alias;
    std::string hostname;
    std::string user;
    std::string port;
    std::string identity;
    std::string proxyjump;
};

inline bool sameFields(const Conn& a, const Conn& b)
{
    return a.alias == b.alias && a.hostname == b.hostname && a.user == b.user && a.port == b.port
           && a.identity == b.identity && a.proxyjump == b.proxyjump;
}

// The file's line ending, decided once from the text the editor read. Only
// changed blocks are regenerated, and those use this style; untouched lines
// keep their exact bytes because they are written from the raw text that was
// parsed. (Same rule as the hosts editor: the dominant style wins.)
inline std::string eolOf(const std::string& text)
{
    if (text.find('\n') == std::string::npos)
        return "\r\n";
    return text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

struct Block {
    BlockType type = BlockType::Host;
    std::string patterns; // the text after "Host" / "Match" / "Include"
    std::string headerRaw; // original section header, preserved for untouched blocks
    std::vector<std::string> gap; // verbatim blank/comment lines before the header
    std::vector<Directive> directives;
    std::vector<std::string> tail; // verbatim lines after the block
    bool dirty = false;
    bool isNew = false;
    bool deleted = false;

    // Host blocks only.
    Conn fields;
    Conn orig; // as parsed, for the change list
};

inline bool isLocked(const Block& b)
{
    if (b.type != BlockType::Host)
        return true;
    return b.patterns.find('*') != std::string::npos || b.patterns.find('?') != std::string::npos;
}

inline bool isEditable(const Block& b) { return !isLocked(b); }

// Known Host keywords, in the order the form writes them.
inline const char* const kKnownHostKeys[] = {"hostname", "user", "port", "identityfile", "proxyjump"};

inline bool isKnownHostKey(const std::string& k)
{
    for (const char* known : kKnownHostKeys)
        if (k == known)
            return true;
    return false;
}

// One line of the written file, for the preview.
struct Line {
    std::string text;
    bool changed = false;
    bool blank = false;
    int connection = -1;
};

enum class ChangeKind { Add, Modify, Delete };

struct FieldChange {
    std::string field; // "alias" | "hostname" | "user" | "port" | "identity" | "proxyjump"
    std::string from;
    std::string to;
};

struct Change {
    ChangeKind kind = ChangeKind::Modify;
    std::string alias; // the block's display name
    std::vector<FieldChange> fields;
};

// Appends the lines of one block, in file order. Shared by Config::lines(),
// the preview and the single-connection copy.
inline void appendBlockLines(const Block& b, std::vector<Line>& out, int connection = -1)
{
    if (b.deleted)
        return;
    for (const std::string& g : b.gap)
        out.push_back(Line {g, false, g.empty(), connection});
    const bool mark = b.dirty || b.isNew;
    switch (b.type) {
    case BlockType::Preamble:
        for (const Directive& d : b.directives)
            out.push_back(Line {d.raw, b.dirty, d.raw.empty(), connection});
        break;
    case BlockType::Include:
        out.push_back(Line {(!mark && !b.headerRaw.empty()) ? b.headerRaw : "Include " + b.patterns, b.dirty, false, connection});
        for (const Directive& d : b.directives)
            out.push_back(Line {d.raw, b.dirty, d.raw.empty(), connection});
        break;
    case BlockType::Match:
        out.push_back(Line {(!mark && !b.headerRaw.empty()) ? b.headerRaw : "Match " + b.patterns, b.dirty, false, connection});
        for (const Directive& d : b.directives)
            out.push_back(Line {d.raw, b.dirty, d.raw.empty(), connection});
        break;
    case BlockType::Host:
        if (!mark) {
            out.push_back(Line {!b.headerRaw.empty() ? b.headerRaw : "Host " + b.patterns, false, false, connection});
            for (const Directive& d : b.directives)
                out.push_back(Line {d.raw, false, d.raw.empty(), connection});
        } else {
            const std::string alias = trim(b.fields.alias);
            out.push_back(Line {"Host " + alias, true, false, connection});
            const std::pair<const char*, std::string> rows[] = {
                {"HostName", trim(b.fields.hostname)},
                {"User", trim(b.fields.user)},
                {"Port", trim(b.fields.port)},
                {"IdentityFile", trim(b.fields.identity)},
                {"ProxyJump", trim(b.fields.proxyjump)},
            };
            for (std::size_t i = 0; i < 5; ++i) {
                const auto& r = rows[i];
                if (!r.second.empty())
                    out.push_back(Line {"    " + std::string(r.first) + " " + r.second, true, false, connection});
            }
            // Unknown directives keep their text and their order.
            for (const Directive& d : b.directives)
                if (!isKnownHostKey(d.key))
                    out.push_back(Line {d.raw, true, false, connection});
        }
        break;
    }
    for (const std::string& t : b.tail)
        out.push_back(Line {t, false, t.empty(), connection});
}

// The whole file: the blocks plus the text they came from.
class Config {
public:
    static Config parse(const std::string& text)
    {
        Config c;
        c.m_base = text;
        c.m_eol = eolOf(text);
        c.m_blocks = parseBlocks(text);
        return c;
    }

    static Config empty()
    {
        Config c;
        c.m_base.clear();
        c.m_eol = defaultEol();
        return c;
    }

    const std::string& base() const { return m_base; }
    const std::string& eol() const { return m_eol; }
    bool hasFile() const { return !m_base.empty(); }

    const std::vector<Block>& blocks() const { return m_blocks; }

    // Indexes of the blocks the table shows (everything but the preamble).
    std::vector<int> connectionIndexes() const
    {
        std::vector<int> out;
        for (int i = 0; i < static_cast<int>(m_blocks.size()); ++i)
            if (m_blocks[static_cast<std::size_t>(i)].type != BlockType::Preamble)
                out.push_back(i);
        return out;
    }

    const Block* connection(int i) const
    {
        const std::vector<int> idx = connectionIndexes();
        if (i < 0 || i >= static_cast<int>(idx.size()))
            return nullptr;
        return &m_blocks[static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])];
    }

    int blockIndex(int connectionIndex) const
    {
        const std::vector<int> idx = connectionIndexes();
        if (connectionIndex < 0 || connectionIndex >= static_cast<int>(idx.size()))
            return -1000000;
        return idx[static_cast<std::size_t>(connectionIndex)];
    }

    bool dirty() const
    {
        for (const Block& b : m_blocks)
            if (b.dirty || b.isNew || b.deleted)
                return true;
        return false;
    }

    int dirtyCount() const
    {
        int n = 0;
        for (const Block& b : m_blocks)
            if (b.dirty || b.isNew || b.deleted)
                ++n;
        return n;
    }

    // ---- writing --------------------------------------------------------------

    std::string serialize() const
    {
        std::vector<Line> out = lines();
        std::string s;
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (i)
                s += m_eol;
            s += out[i].text;
        }
        return s;
    }

    std::vector<Line> lines() const
    {
        std::vector<Line> out;
        int connection = -1;
        for (const Block& b : m_blocks) {
            if (b.type != BlockType::Preamble)
                ++connection;
            appendBlockLines(b, out, b.type == BlockType::Preamble ? -1 : connection);
        }
        return out;
    }

    // ---- change list ----------------------------------------------------------

    std::vector<Change> changes() const
    {
        std::vector<Change> out;
        for (const Block& b : m_blocks) {
            if (!b.dirty && !b.isNew && !b.deleted)
                continue;
            Change c;
            const std::string alias = trim(b.fields.alias).empty() ? b.patterns : trim(b.fields.alias);
            c.alias = alias;
            if (b.deleted) {
                c.kind = ChangeKind::Delete;
            } else if (b.isNew) {
                c.kind = ChangeKind::Add;
            } else {
                c.kind = ChangeKind::Modify;
                const std::pair<const char*, std::pair<std::string, std::string>> pairs[] = {
                    {"alias", {b.orig.alias, b.fields.alias}},
                    {"hostname", {b.orig.hostname, b.fields.hostname}},
                    {"user", {b.orig.user, b.fields.user}},
                    {"port", {b.orig.port, b.fields.port}},
                    {"identity", {b.orig.identity, b.fields.identity}},
                    {"proxyjump", {b.orig.proxyjump, b.fields.proxyjump}},
                };
                for (const auto& p : pairs)
                    if (p.second.first != p.second.second)
                        c.fields.push_back(FieldChange {p.first, p.second.first, p.second.second});
                if (c.fields.empty())
                    continue; // dirty but identical: nothing to report
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    // ---- editing --------------------------------------------------------------

    // Adds a blank connection above the trailing read-only blocks (global /
    // Match / Include belong at the bottom of the file). Returns the connection
    // index of the new row.
    int addConnection()
    {
        snapshot();
        Block b;
        b.type = BlockType::Host;
        b.patterns.clear();
        b.fields.port = "22";
        b.orig = b.fields;
        b.dirty = true;
        b.isNew = true;

        // New connections go above the trailing global / Match / Include blocks,
        // which are read-only and belong at the bottom of the file. A leading
        // preamble (free-standing top-of-file lines) stays first: its directives
        // are global and must remain above every Host block.
        int insertAt = static_cast<int>(m_blocks.size());
        int scanFrom = 0;
        if (!m_blocks.empty() && m_blocks.front().type == BlockType::Preamble) {
            scanFrom = 1;
            insertAt = 1;
        }
        for (int i = scanFrom; i < static_cast<int>(m_blocks.size()); ++i) {
            if (isLocked(m_blocks[static_cast<std::size_t>(i)]) && m_blocks[static_cast<std::size_t>(i)].type != BlockType::Preamble) {
                insertAt = i;
                break;
            }
        }

        if (insertAt == 0 && !m_blocks.empty()) {
            // The first block's leading lines are the file header, and may hold
            // global directives. Move them onto the new block so they still
            // precede the first Host instead of becoming that Host's settings.
            b.gap = m_blocks.front().gap;
            m_blocks.front().gap.clear();
        }
        if (b.gap.empty() || !trim(b.gap.back()).empty())
            b.gap.push_back(std::string());
        m_blocks.insert(m_blocks.begin() + insertAt, std::move(b));

        // The new block's connection index.
        int conn = 0;
        for (int i = 0; i < insertAt; ++i)
            if (m_blocks[static_cast<std::size_t>(i)].type != BlockType::Preamble)
                ++conn;
        return conn;
    }

    // Returns false when the block is read-only (the caller reports why).
    bool setField(int connectionIndex, const std::string& field, const std::string& value)
    {
        const int bi = blockIndex(connectionIndex);
        if (bi < 0)
            return false;
        Block& b = m_blocks[static_cast<std::size_t>(bi)];
        if (!isEditable(b) || b.deleted)
            return false;
        if (field == "alias")
            b.fields.alias = value;
        else if (field == "hostname")
            b.fields.hostname = value;
        else if (field == "user")
            b.fields.user = value;
        else if (field == "port")
            b.fields.port = value;
        else if (field == "identity")
            b.fields.identity = value;
        else if (field == "proxyjump")
            b.fields.proxyjump = value;
        else
            return false;
        // Editing a value back to what the file already had leaves the block
        // clean again, exactly like the change list.
        b.dirty = b.isNew || !sameFields(b.fields, b.orig);
        return true;
    }

    // Begins an edit step: call once before a batch of setField() calls so a
    // single form change is one undo step.
    void beginEdit() { snapshot(); }

    // Marks a row for deletion (and back). New rows are dropped outright.
    bool toggleDelete(int connectionIndex)
    {
        const int bi = blockIndex(connectionIndex);
        if (bi < 0)
            return false;
        Block& b = m_blocks[static_cast<std::size_t>(bi)];
        if (!isEditable(b))
            return false;
        if (b.isNew) {
            snapshot();
            m_blocks.erase(m_blocks.begin() + bi);
            return true;
        }
        snapshot();
        b.deleted = !b.deleted;
        return true;
    }

    int deleteMany(std::vector<int> connectionIndexes)
    {
        std::sort(connectionIndexes.begin(), connectionIndexes.end());
        connectionIndexes.erase(std::unique(connectionIndexes.begin(), connectionIndexes.end()), connectionIndexes.end());
        std::vector<int> blocks;
        for (const int index : connectionIndexes) {
            const int bi = blockIndex(index);
            if (bi >= 0 && isEditable(m_blocks[static_cast<std::size_t>(bi)])
                && !m_blocks[static_cast<std::size_t>(bi)].deleted)
                blocks.push_back(bi);
        }
        if (blocks.empty())
            return 0;
        snapshot();
        int deleted = 0;
        for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
            Block& block = m_blocks[static_cast<std::size_t>(*it)];
            if (block.isNew)
                m_blocks.erase(m_blocks.begin() + *it);
            else
                block.deleted = true;
            ++deleted;
        }
        return deleted;
    }

    // ---- validation -----------------------------------------------------------

    // Field name -> error code ("E_ALIAS" / "E_HOSTNAME" / "E_PORT").
    std::map<std::string, std::string> validate(int connectionIndex) const
    {
        std::map<std::string, std::string> errs;
        const Block* b = connection(connectionIndex);
        if (!b || !isEditable(*b) || b->deleted)
            return errs;
        const std::string alias = trim(b->fields.alias);
        if (alias.empty() || alias.find(' ') != std::string::npos
            || alias.find('\t') != std::string::npos || alias.find('*') != std::string::npos
            || alias.find('?') != std::string::npos)
            errs["alias"] = "E_ALIAS";
        if (trim(b->fields.hostname).empty())
            errs["hostname"] = "E_HOSTNAME";
        const std::string port = trim(b->fields.port);
        if (!port.empty()) {
            bool digits = !port.empty() && port.size() <= 5;
            for (char c : port)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    digits = false;
            const long n = digits ? std::stol(port) : 0;
            if (!digits || n < 1 || n > 65535)
                errs["port"] = "E_PORT";
        }
        return errs;
    }

    bool anyInvalid() const
    {
        const std::vector<int> idx = connectionIndexes();
        for (int i = 0; i < static_cast<int>(idx.size()); ++i)
            if (!validate(i).empty())
                return true;
        return false;
    }

    // ---- undo / redo ----------------------------------------------------------

    void snapshot()
    {
        m_undo.push_back(m_blocks);
        if (m_undo.size() > 100)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    bool undo()
    {
        if (m_undo.empty())
            return false;
        m_redo.push_back(m_blocks);
        m_blocks = m_undo.back();
        m_undo.pop_back();
        return true;
    }

    bool redo()
    {
        if (m_redo.empty())
            return false;
        m_undo.push_back(m_blocks);
        m_blocks = m_redo.back();
        m_redo.pop_back();
        return true;
    }

    void clearHistory()
    {
        m_undo.clear();
        m_redo.clear();
    }

    // After a successful write: re-read what was written, exactly like the
    // window does, so field values and raw directives stay consistent.
    void reload(const std::string& text)
    {
        m_base = text;
        m_eol = eolOf(text);
        m_blocks = parseBlocks(text);
        clearHistory();
    }

    // ---- parsing --------------------------------------------------------------

    static std::string defaultEol() { return "\r\n"; }

    static std::vector<Block> parseBlocks(const std::string& text);

private:
    std::string m_base;
    std::string m_eol = defaultEol();
    std::vector<Block> m_blocks;
    std::vector<std::vector<Block>> m_undo;
    std::vector<std::vector<Block>> m_redo;
};

inline Conn fieldsFromDirectives(const Block& b)
{
    Conn c;
    c.alias = b.patterns;
    for (const Directive& d : b.directives) {
        if (d.key == "hostname")
            c.hostname = d.value;
        else if (d.key == "user")
            c.user = d.value;
        else if (d.key == "port")
            c.port = d.value;
        else if (d.key == "identityfile")
            c.identity = d.value;
        else if (d.key == "proxyjump")
            c.proxyjump = d.value;
    }
    return c;
}

inline std::vector<Block> Config::parseBlocks(const std::string& text)
{
    std::vector<Block> blocks;
    if (text.empty())
        return blocks;

    std::vector<std::string> pending; // blank/comment lines, kept for the next block
    Block cur;
    bool open = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();

        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') {
            pending.push_back(raw);
            continue;
        }

        std::size_t k = 0;
        while (k < line.size() && (std::isalnum(static_cast<unsigned char>(line[k])) || line[k] == '-'
                                   || line[k] == '_'))
            ++k;
        const std::string key = toLower(line.substr(0, k));
        std::string value = trim(line.substr(k));

        if (key == "host" || key == "match" || key == "include") {
            if (open)
                blocks.push_back(cur);
            cur = Block {};
            cur.type = key == "host" ? BlockType::Host
                       : key == "match" ? BlockType::Match
                                        : BlockType::Include;
            cur.headerRaw = raw;
            if (key == "host") {
                const std::size_t comment = value.find('#');
                if (comment != std::string::npos)
                    value = trim(value.substr(0, comment));
            }
            cur.patterns = value;
            // Blank and comment lines between two sections describe the section
            // that follows them: keeping them as the next block's gap means a
            // deleted or inserted neighbour never swallows or adopts them.
            cur.gap = pending;
            pending.clear();
            open = true;
            continue;
        }

        if (open) {
            Directive d;
            d.key = key;
            d.value = value;
            d.raw = raw;
            cur.directives.push_back(std::move(d));
        } else {
            // A directive before any header: keep it verbatim, in file order.
            pending.push_back(raw);
        }
    }

    if (open) {
        if (!pending.empty()) {
            cur.tail.insert(cur.tail.end(), pending.begin(), pending.end());
            pending.clear();
        }
        blocks.push_back(cur);
    }
    if (!pending.empty()) {
        Block p;
        p.type = BlockType::Preamble;
        for (const std::string& l : pending) {
            Directive d;
            d.raw = l;
            p.directives.push_back(std::move(d));
        }
        blocks.push_back(std::move(p));
    }

    // Materialise the editable fields and remember what was parsed.
    for (Block& b : blocks) {
        if (b.type != BlockType::Host)
            continue;
        b.fields = fieldsFromDirectives(b);
        b.orig = b.fields;
        b.dirty = false;
        b.isNew = false;
        b.deleted = false;
    }
    return blocks;
}

// Convenience wrapper: keeps the call sites short in the bridge and the tests.
inline Config parseConfig(const std::string& text) { return Config::parse(text); }

// One block rendered as a standalone line range (used by "copy line"). The
// caller passes the file's line ending.
inline std::string serializeBlock(const Block& b, const std::string& eol)
{
    std::vector<Line> out;
    appendBlockLines(b, out);
    std::string s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i)
            s += eol;
        s += out[i].text;
    }
    return s;
}

// ---- connection helpers -------------------------------------------------------

inline std::string displayName(const Block& b)
{
    if (b.type == BlockType::Host)
        return trim(b.fields.alias).empty() ? b.patterns : trim(b.fields.alias);
    if (b.type == BlockType::Match)
        return "Match " + b.patterns;
    if (b.type == BlockType::Include)
        return "Include " + b.patterns;
    return {};
}

// "~/.ssh/id_ed25519" -> "id_ed25519"
inline std::string keyNameFromPath(const std::string& path)
{
    std::string p = trim(path);
    std::size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos)
        p = p.substr(slash + 1);
    return p;
}

// Does the connection use this key file? Compares the file name only, so
// "~/.ssh/id_ed25519", "%d/.ssh/id_ed25519" and "id_ed25519" all match.
inline bool usesKey(const Block& b, const std::string& keyName)
{
    if (b.type != BlockType::Host || b.deleted)
        return false;
    return keyNameFromPath(b.fields.identity) == keyName;
}

inline bool connMatches(const Block& b, const std::string& filter)
{
    const std::string hay = toLower(displayName(b) + " " + b.patterns + " " + b.fields.hostname + " "
                                    + b.fields.user + " " + b.fields.port + " " + b.fields.identity
                                    + " " + b.fields.proxyjump);
    return fuzzyScore(hay, filter) > -1000000;
}

inline bool keyMatches(const KeyFile& k, const std::string& filter)
{
    if (filter.empty())
        return true;
    return fuzzyScore(k.name + " " + k.pub.type + " " + k.pub.comment + " " + k.pub.fingerprint, filter) > -1000000;
}

} // namespace ssh
} // namespace envh
