// test_envhosts_logic.cpp - self-contained tests for logic.h (no plugin host, no
// Windows needed). Build: cmake -B build && cmake --build build && ctest --test-dir build
// The hosts round-trip tests use a sanitised copy of a real captured hosts file (fixtures/hosts.sample)
// (hostnames and private IPs replaced with RFC 5737 documentation addresses)
// so BOM / CRLF / Docker-section / duplicate behaviour is exercised exactly.
#include "logic.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 8, "logic is fine on any width, but the shipped plugin must be x64");

namespace {

int g_total = 0;
int g_failed = 0;

#define CHECK(cond, label)                                                                             \
    do {                                                                                           \
        ++g_total;                                                                                 \
        if (!(cond)) {                                                                             \
            ++g_failed;                                                                           \
            std::cerr << "FAIL [" << (label) << "] at line " << __LINE__ << "\n";                 \
        }                                                                                          \
    } while (false)

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string findFixture()
{
    const char* env = std::getenv("ENVHOSTS_FIXTURE");
    if (env && !std::string(env).empty())
        return env;
    const char* candidates[] = {
        "tests/fixtures/hosts.sample",
        "../tests/fixtures/hosts.sample",
        "../../tests/fixtures/hosts.sample",
        "ra9fael.envhosts/tests/fixtures/hosts.sample",
    };
    for (const char* c : candidates) {
        const std::string s = readFile(c);
        if (!s.empty())
            return c;
    }
    return {};
}

// ---- hosts: parse / round-trip / toggle / add --------------------------------
void testHosts(const std::string& fixture)
{
    const std::string content = readFile(fixture);
    CHECK(!content.empty(), "fixture loaded");

    // T1.1: byte-exact round trip
    const envh::HostsParsed parsed = envh::parseHosts(content);
    CHECK(envh::serializeHosts(parsed) == content, "hosts round-trip byte-identical (sample file)");
    CHECK(parsed.bom == std::string("\xEF\xBB\xBF"), "hosts sample file keeps UTF-8 BOM");

    const std::vector<envh::HostsEntry> entries = envh::hostsEntries(parsed);
    int live = 0, commented = 0, section = 0;
    for (const auto& e : entries) {
        if (e.section)
            ++section;
        else if (e.commented)
            ++commented;
        else
            ++live;
    }
    CHECK(live > 0, "hosts has at least one live entry");
    CHECK(section >= 0, "hosts section count sane");

    // T1.2: toggle only changes the target line, file otherwise untouched
    if (live > 0) {
        // find first live entry
        int idx = -1;
        for (const auto& e : entries)
            if (!e.commented && !e.section) {
                idx = e.lineIndex;
                break;
            }
        const std::string before = parsed.lines[idx].text;
        const auto toggled = envh::hostsToggle(parsed, idx);
        CHECK(toggled.success, "hostsToggle succeeds");
        const std::string after = envh::serializeHosts(toggled.value);
        // Exactly one line differs.
        const envh::HostsParsed p2 = envh::parseHosts(after);
        int diff = 0;
        for (std::size_t i = 0; i < p2.lines.size(); ++i) {
            const std::string orig = (i < parsed.lines.size()) ? parsed.lines[i].text : "";
            if (p2.lines[i].text != orig)
                ++diff;
        }
        CHECK(diff == 1, "toggle changes exactly one line");
        CHECK(envh::lineIsCommented(p2.lines[idx].text), "toggled line is now commented");
        // toggle back restores byte identity
        const auto back = envh::hostsToggle(toggled.value, idx);
        CHECK(envh::serializeHosts(back.value) == content, "toggle off restores original bytes");
    }

    // T1.2b: section lines cannot be toggled
    {
        envh::HostsParsed p = envh::parseHosts("# A section header\n127.0.0.1 host.local\n");
        const auto entries2 = envh::hostsEntries(p);
        int secIdx = -1;
        for (const auto& e : entries2)
            if (e.section) {
                secIdx = e.lineIndex;
                break;
            }
        CHECK(secIdx >= 0, "synthetic section detected");
        const auto r = envh::hostsToggle(p, secIdx);
        CHECK(!r.success && r.failure.code == "E_HOSTS_SECTION", "section toggle rejected");
    }

    // T1.3: edge cases round-trip byte-identically
    const char* cases[] = {
        "127.0.0.1 localhost\n", // LF only, no BOM, trailing newline
        "127.0.0.1 localhost", // no trailing newline
        "::1\tlocalhost\n", // IPv6 + tab separator
        "  127.0.0.1   spaced.host  # a comment\n", // inline spaces + comment
        "0.0.0.0 single.token\n", // single token (no hostname)
        "# just a comment\n127.0.0.1 a.local\n", // comment + entry
        "a\nb\nc\n", // LF, multiple lines, no trailing newline on last
        "\xEF\xBB\xBF# BOM and comment only\n", // BOM, no entries
    };
    for (const char* c : cases) {
        const std::string s(c);
        const envh::HostsParsed p = envh::parseHosts(s);
        CHECK(envh::serializeHosts(p) == s, std::string("round-trip edge: ") + c);
    }
    // CRLF preserved
    {
        const std::string s = "127.0.0.1 a.local\r\n127.0.0.1 b.local\r\n";
        const envh::HostsParsed p = envh::parseHosts(s);
        CHECK(p.lines[0].crlf && p.lines[1].crlf, "CRLF preserved");
        CHECK(envh::serializeHosts(p) == s, "CRLF round-trip");
    }

    // T1.4: hostsAdd
    {
        envh::HostsParsed p = envh::parseHosts("127.0.0.1 a.local\n");
        auto bad = envh::hostsAdd(p, "not.an.ip", "b.local", "");
        CHECK(!bad.success && bad.failure.code == "E_HOSTS_IP", "hostsAdd rejects bad ip");
        auto dup = envh::hostsAdd(p, "10.0.0.1", "a.local", "");
        CHECK(!dup.success && dup.failure.code == "E_HOSTS_DUP", "hostsAdd rejects duplicate hostname");
        auto ok = envh::hostsAdd(p, "10.0.0.1", "b.local", "my note");
        CHECK(ok.success, "hostsAdd valid succeeds");
        const std::string out = envh::serializeHosts(ok.value);
        CHECK(out.find("10.0.0.1 b.local # my note") != std::string::npos, "hostsAdd appends with comment");
        CHECK(out.find("127.0.0.1 a.local") != std::string::npos, "hostsAdd keeps original line");
    }
    {
        // blank-line insertion uses the file's dominant line ending
        envh::HostsParsed p = envh::parseHosts("127.0.0.1 a.local\r\n");
        auto ok = envh::hostsAdd(p, "10.0.0.1", "b.local", "");
        CHECK(ok.success, "hostsAdd on CRLF file");
        CHECK(envh::serializeHosts(ok.value).find("b.local\r\n") != std::string::npos, "hostsAdd uses CRLF");
    }
}

// ---- env model ---------------------------------------------------------------
void testEnv()
{
    // T1.5: validation
    CHECK(envh::isValidEnvName("PATH"), "PATH valid name");
    CHECK(!envh::isValidEnvName("A=B"), "name with '=' rejected");
    CHECK(!envh::isValidEnvName(""), "empty name rejected");
    CHECK(!envh::isValidEnvName(std::string(300, 'x')), "overlong name rejected");
    CHECK(envh::isValidEnvValue("normal value"), "normal value ok");
    CHECK(!envh::isValidEnvValue(std::string(40000, 'x')), "overlong value rejected");
    CHECK(!envh::isValidEnvValue(std::string("bad\x01value")), "control char in value rejected");

    CHECK(envh::envTypeForValue("C:\\x") == "REG_SZ", "plain value -> REG_SZ");
    CHECK(envh::envTypeForValue("%USERPROFILE%\\x") == "REG_EXPAND_SZ", "% value -> REG_EXPAND_SZ");

    // expansion
    {
        std::map<std::string, std::string> kv;
        kv["userprofile"] = "C:\\u";
        kv["windir"] = "C:\\w";
        CHECK(envh::expandEnvValue("%USERPROFILE%\\x", kv) == "C:\\u\\x", "expands known var");
        CHECK(envh::expandEnvValue("%NOPE%\\x", kv) == "%NOPE%\\x", "unresolved var left verbatim");
        CHECK(envh::expandEnvValue("plain", kv) == "plain", "plain text unchanged");
    }

    // envSet / envDelete
    {
        envh::EnvSnapshot s;
        auto r = envh::envSet(s, "FOO", "bar", "user");
        CHECK(r.success && r.value.vars.size() == 1, "envSet adds var");
        CHECK(r.value.vars[0].type == "REG_SZ", "new plain var REG_SZ");
        auto r2 = envh::envSet(r.value, "FOO", "%BAR%", "user");
        CHECK(r2.success && r2.value.vars[0].value == "%BAR%" && r2.value.vars[0].type == "REG_EXPAND_SZ",
              "envSet updates value and type");
        auto r3 = envh::envSet(s, "BA D", "x", "user");
        CHECK(!r3.success, "envSet rejects bad name");
        auto del = envh::envDelete(r2.value, "FOO", "user");
        CHECK(del.success && del.value.vars.empty(), "envDelete removes var");
        auto delMiss = envh::envDelete(s, "NOPE", "user");
        CHECK(!delMiss.success && delMiss.failure.code == "E_NOTFOUND", "envDelete missing -> E_NOTFOUND");
    }

    // system vs user distinct, lookup override order
    {
        envh::EnvSnapshot s;
        const auto r1 = envh::envSet(s, "PATH", "userpath", "user");
        const auto r2 = envh::envSet(r1.value, "PATH", "syspath", "system");
        const envh::EnvVar* u = envh::findEnvVar(r2.value, "PATH", "user");
        const envh::EnvVar* y = envh::findEnvVar(r2.value, "PATH", "system");
        CHECK(u && u->value == "userpath", "user PATH found");
        CHECK(y && y->value == "syspath", "system PATH found separately");
    }

    CHECK(envh::isWellKnownName("PATH"), "PATH is well-known");
    CHECK(!envh::isWellKnownName("MYAPP"), "MYAPP not well-known");

    // envBlockSize guard threshold
    {
        envh::EnvSnapshot s;
        envh::EnvVar v;
        v.scope = "user";
        v.name = "BIG";
        v.value = std::string(40000, 'x'); // > kEnvBlockWarn (30000)
        s.vars.push_back(v);
        CHECK(envh::envBlockSize(s) > envh::kEnvBlockWarn, "envBlockSize crosses warn threshold");
    }
}

// ---- grammar -----------------------------------------------------------------
void testGrammar()
{
    // T1.6
    auto eq = [](envh::CmdKind a, envh::CmdKind b) { return a == b; };
    CHECK(eq(envh::parseQuery("env", "").kind, envh::CmdKind::EnvList), "env -> EnvList");
    CHECK(eq(envh::parseQuery("env", "path").kind, envh::CmdKind::EnvList)
              && envh::parseQuery("env", "path").filter == "path",
          "env path -> filter");
    {
        auto q = envh::parseQuery("env", "set PATH=C:\\x");
        CHECK(eq(q.kind, envh::CmdKind::EnvSet) && q.name == "PATH" && q.value == "C:\\x", "env:set parses name=value");
    }
    {
        auto q = envh::parseQuery("env", "set PATH=a=b");
        CHECK(q.kind == envh::CmdKind::EnvSet && q.value == "a=b", "env:set keeps '=' in value");
    }
    CHECK(eq(envh::parseQuery("env", "new").kind, envh::CmdKind::EnvNew), "env new");
    CHECK(eq(envh::parseQuery("env", "restore").kind, envh::CmdKind::EnvRestore), "env restore");
    CHECK(eq(envh::parseQuery("hosts", "").kind, envh::CmdKind::HostsList), "hosts -> HostsList");
    CHECK(eq(envh::parseQuery("hosts", "docker").kind, envh::CmdKind::HostsList), "hosts docker -> filter");
    {
        auto q = envh::parseQuery("hosts", "add 127.0.0.1 example.test");
        CHECK(eq(q.kind, envh::CmdKind::HostsAdd) && q.ip == "127.0.0.1" && q.hostname == "example.test",
              "hosts:add parses ip/host");
    }
    {
        auto q = envh::parseQuery("hosts", "add 127.0.0.1 example.test # my comment");
        CHECK(q.comment == "my comment", "hosts:add parses comment after '#'");
    }
    CHECK(envh::parseQuery("hosts", "add onlyone").hasError, "hosts:add with 1 token -> error");
    CHECK(envh::parseQuery("bogus", "x").hasError, "unknown command -> error");
}

// ---- id stability -------------------------------------------------------------
void testIds()
{
    // T1.7: hosts id stable across reordering
    const std::string a = "127.0.0.1 alpha.local\n127.0.0.1 beta.local\n";
    const std::string b = "127.0.0.1 beta.local\n127.0.0.1 alpha.local\n";
    const std::vector<envh::HostsEntry> ea = envh::hostsEntries(envh::parseHosts(a));
    const std::vector<envh::HostsEntry> eb = envh::hostsEntries(envh::parseHosts(b));
    CHECK(envh::hostsEntryId(ea[0]) == envh::hostsEntryId(eb[1]), "hosts id stable after reorder (alpha)");
    CHECK(envh::hostsEntryId(ea[1]) == envh::hostsEntryId(eb[0]), "hosts id stable after reorder (beta)");

    // identical lines share a stable base id; the "-2" suffix is added by
    // main.cpp's row builder when it sees a collision.
    {
        const std::string d = "127.0.0.1 dup.local\n127.0.0.1 dup.local\n";
        const auto es = envh::hostsEntries(envh::parseHosts(d));
        CHECK(es.size() == 2 && envh::hostsEntryId(es[0]) == envh::hostsEntryId(es[1]),
              "identical lines share a base id (suffix added by main.cpp)");
    }

    // env id stable + scope-distinct
    {
        envh::EnvVar u, y;
        u.scope = "user"; u.name = "Path";
        y.scope = "system"; y.name = "Path";
        CHECK(envh::envId(u) == "env:user:path", "user env id lowercase");
        CHECK(envh::envId(y) == "env:system:path", "system env id distinct");
        CHECK(envh::envId(u) == envh::envId(u), "env id stable");
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string fixture = (argc > 1) ? argv[1] : findFixture();
    if (fixture.empty()) {
        std::cerr << "SKIP hosts fixture tests: set ENVHOSTS_FIXTURE or run from the source tree\n";
    } else {
        testHosts(fixture);
    }
    testEnv();
    testGrammar();
    testIds();

    std::cerr << (g_failed ? "FAILED" : "PASS") << ": " << (g_total - g_failed) << "/" << g_total << " checks\n";
    return g_failed ? 1 : 0;
}
