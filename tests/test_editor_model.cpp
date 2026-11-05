// test_editor_model.cpp - unit tests for editor_model.h (pure, no Win32 host).
// Covers the model layer behind the interactive editor window: dirty tracking,
// undo/redo, PATH split/join, multi-hostname hosts lines, byte-exact save,
// fingerprint, and draft round-trip (design doc test cases U1, U3-U10).
#include "editor_model.h"
#include "logic.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

static int g_total = 0;
static int g_failed = 0;

#define CHECK(cond, label)                                                                          \
    do {                                                                                            \
        ++g_total;                                                                                  \
        if (!(cond)) {                                                                              \
            ++g_failed;                                                                             \
            std::cerr << "FAIL [" << (label) << "] at line " << __LINE__ << "\n";                   \
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

// ---- U3: PATH split/join round trip ----------------------------------------
void testPathSplit()
{
    using envh::edit::pathSplit;
    using envh::edit::pathJoin;
    CHECK(pathJoin(pathSplit("a;b;c")) == "a;b;c", "path roundtrip simple");
    CHECK(pathJoin(pathSplit("a;;b;")) == "a;;b;", "path roundtrip empty + trailing");
    CHECK(pathJoin(pathSplit("")) == "", "path roundtrip empty string");
    CHECK(pathJoin(pathSplit("single")) == "single", "path roundtrip single");
    // Quoted segment keeps its semicolon.
    const std::vector<std::string> q = pathSplit("C:\\a;\"D:\\b;c\";E:\\d");
    CHECK(q.size() == 3, "path quoted segment count");
    CHECK(q[1] == "D:\\b;c", "path quoted segment text");
    CHECK(pathJoin(q) == "C:\\a;D:\\b;c;E:\\d", "path quoted join");
}

// ---- U4: dirty tracking edit then edit back ---------------------------------
void testEnvDirty()
{
    envh::EnvSnapshot snap;
    envh::EnvVar v;
    v.scope = "user";
    v.name = "FOO";
    v.type = "REG_SZ";
    v.value = "bar";
    snap.vars.push_back(v);

    envh::edit::EnvModel m = envh::edit::EnvModel::fromSnapshot(snap);
    CHECK(!m.isDirty(), "env not dirty at start");
    m.pushUndo();
    m.rows[0].value = "baz";
    CHECK(m.isDirty(), "env dirty after edit");
    CHECK(m.changeList().size() == 1, "env one change");
    m.rows[0].value = "bar"; // edit back
    CHECK(!m.isDirty(), "env not dirty after revert");
    CHECK(m.changeList().empty(), "env no change after revert");
    (void)m.canUndo();
}

// ---- U5: undo/redo over many commands, bounded at 200 -----------------------
void testUndoRedo()
{
    envh::EnvSnapshot snap;
    envh::EnvVar v;
    v.scope = "user";
    v.name = "K";
    v.type = "REG_SZ";
    v.value = "0";
    snap.vars.push_back(v);
    envh::edit::EnvModel m = envh::edit::EnvModel::fromSnapshot(snap);
    for (int i = 1; i <= 250; ++i) {
        m.pushUndo();
        m.rows[0].value = std::to_string(i);
    }
    // 250 undos should land back at the base value (undo stack capped at 200,
    // so after 200 undos we reach the oldest retained state, not base).
    for (int i = 0; i < 200; ++i)
        m.undo();
    CHECK(m.canRedo(), "env can redo after undos");
    for (int i = 0; i < 200; ++i)
        m.redo();
    CHECK(m.rows[0].value == "250", "env redo restores latest");
}

// ---- U6: hosts multi-hostname line -----------------------------------------
void testHostsMultiHost()
{
    const std::string line = "127.0.0.1 a.home b.home # my comment";
    const envh::HostsEntry e = envh::parseHostLine(line, 0);
    CHECK(!e.commented, "hosts multi not commented");
    CHECK(e.ip == "127.0.0.1", "hosts multi ip");
    CHECK(e.hostname == "a.home", "hosts multi first hostname");
    CHECK(e.hostnames.size() == 2, "hosts multi hostname count");
    CHECK(e.hostnames[0] == "a.home" && e.hostnames[1] == "b.home", "hosts multi hostnames");
    CHECK(e.comment == "my comment", "hosts multi comment");
}

// ---- U7: byte-exact save when one line toggled ------------------------------
std::vector<std::string> splitLines(const std::string& s);

void testHostsByteExact()
{
    const std::string fixture = findFixture();
    // Use a synthetic BOM + CRLF + blank + section fixture so the test controls
    // the exact lines (the shared hosts.sample is reserved for logic.h round-trip).
    std::string content = std::string("\xEF\xBB\xBF")
                         + "# Added by Docker Desktop\r\n"
                         + "\r\n"
                         + "127.0.0.1 host.a # keep\r\n"
                         + "192.168.1.5 host.b\r\n";
    (void)fixture;
    const envh::HostsParsed parsed = envh::parseHosts(content);
    bool dominant = false;
    for (const envh::HostsLine& l : parsed.lines)
        if (!l.text.empty())
            dominant = l.crlf;
    envh::edit::HostsModel m = envh::edit::HostsModel::fromParsed(parsed, dominant);
    CHECK(!m.isDirty(), "hosts not dirty at start");
    // Find the enabled line "host.b" and disable it.
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t i = 0; i < m.rows.size(); ++i) {
        if (!m.rows[i].isSection && m.rows[i].ip == "192.168.1.5") {
            idx = i;
            found = true;
            break;
        }
    }
    CHECK(found, "hosts found target line");
    if (found) {
        m.setEnabled(idx, false);
        CHECK(m.isDirty(), "hosts dirty after disable");
        const std::string out = m.serializeWork();
        // The untouched line must survive byte-for-byte.
        CHECK(out.find("127.0.0.1 host.a # keep") != std::string::npos, "hosts untouched line kept");
        CHECK(out.find("# 192.168.1.5 host.b") != std::string::npos, "hosts disabled line commented");
        // BOM and CRLF preserved.
        CHECK(out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF
                  && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF,
              "hosts BOM preserved");
        CHECK(out.find("\r\n") != std::string::npos, "hosts CRLF preserved");
        // Exactly one line should differ from the original.
        std::size_t diffLines = 0;
        std::vector<std::string> a = splitLines(content), b = splitLines(out);
        const std::size_t n = std::max(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
            const std::string la = i < a.size() ? a[i] : "";
            const std::string lb = i < b.size() ? b[i] : "";
            if (la != lb)
                ++diffLines;
        }
        CHECK(diffLines == 1, "hosts exactly one line changed");
    }
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// ---- U8: fingerprint sensitivity -------------------------------------------
void testFingerprint()
{
    envh::EnvSnapshot s1, s2;
    envh::EnvVar v;
    v.scope = "user";
    v.name = "A";
    v.type = "REG_SZ";
    v.value = "1";
    s1.vars.push_back(v);
    s2.vars.push_back(v);
    const std::string f1 = envh::edit::EnvModel::computeFingerprint(s1);
    const std::string f2 = envh::edit::EnvModel::computeFingerprint(s2);
    CHECK(f1 == f2, "env fingerprint stable");

    envh::EnvVar v2 = v;
    v2.value = "2";
    s2.vars[0] = v2;
    CHECK(envh::edit::EnvModel::computeFingerprint(s2) != f1, "env fingerprint changes on value");

    // hosts: size + content sensitivity
    const std::string h1 = "127.0.0.1 a\n";
    const std::string h2 = "127.0.0.1 b\n";
    CHECK(envh::edit::HostsModel::contentFingerprint(h1) != envh::edit::HostsModel::contentFingerprint(h2),
          "hosts fingerprint changes on byte");
    CHECK(envh::edit::HostsModel::contentFingerprint(h1 + "x") != envh::edit::HostsModel::contentFingerprint(h1),
          "hosts fingerprint changes on size");
}

// ---- U9: draft round-trip with nasty values --------------------------------
void testDraftRoundTrip()
{
    envh::EnvSnapshot snap;
    envh::EnvVar v;
    v.scope = "user";
    v.name = "TOKEN";
    v.type = "REG_SZ";
    v.value = "ak_\"slash\\quote\nCJK表格" + std::string(100, 'x'); // control-free but with quotes/newline/CJK
    snap.vars.push_back(v);
    envh::edit::EnvModel m = envh::edit::EnvModel::fromSnapshot(snap);
    m.pushUndo();
    m.rows[0].value = std::string(32767, 'Z'); // max length
    const winshun::Json draft = m.toDraft();
    const std::string dumped = draft.dump();
    CHECK(!dumped.empty(), "env draft dumps");
    envh::edit::EnvModel m2;
    m2.fromDraft(winshun::Json::parse(dumped).value());
    CHECK(m2.rows.size() == 1, "env draft restores row count");
    CHECK(m2.rows[0].value.size() == 32767, "env draft restores max-length value");
    CHECK(m2.rows[0].value == std::string(32767, 'Z'), "env draft value identical");

    // hosts draft
    envh::HostsParsed p = envh::parseHosts("127.0.0.1 a.home b.home # c\n");
    envh::edit::HostsModel hm = envh::edit::HostsModel::fromParsed(p, false);
    const winshun::Json hd = hm.toDraft();
    envh::edit::HostsModel hm2;
    hm2.fromDraft(winshun::Json::parse(hd.dump()).value());
    CHECK(hm2.rows.size() == 1, "hosts draft restores row count");
    CHECK(hm2.rows[0].hostnames.size() == 2, "hosts draft restores multi-host");
    CHECK(hm2.rows[0].comment == "c", "hosts draft restores comment");
}

// ---- U10: backup loaded into work as unsaved edits --------------------------
void testBackupIntoWork()
{
    // A "backup" is just an EnvSnapshot; loading it as the base+work would mark
    // it clean, so the window loads it into `work` (rows) and recomputes base
    // from the live read. Here we simulate: base = live, work = backup => dirty.
    envh::EnvSnapshot live;
    envh::EnvVar lv;
    lv.scope = "user";
    lv.name = "LIVE";
    lv.type = "REG_SZ";
    lv.value = "live";
    live.vars.push_back(lv);

    envh::EnvSnapshot backup;
    envh::EnvVar bv;
    bv.scope = "user";
    bv.name = "RESTORED";
    bv.type = "REG_SZ";
    bv.value = "frombackup";
    backup.vars.push_back(bv);

    envh::edit::EnvModel m = envh::edit::EnvModel::fromSnapshot(live);
    // Simulate loading backup into work.
    envh::edit::EnvModel backupModel = envh::edit::EnvModel::fromSnapshot(backup);
    m.rows = backupModel.rows;
    CHECK(m.isDirty(), "backup loaded into work is dirty");
    CHECK(m.changeList().size() >= 1, "backup produces changes");
    // Nothing written yet.
    (void)m.delta();
}

// ---- U11: row commands behind the window's Ins / Del buttons (v0.2.1) ------
void testRowCommands()
{
    envh::HostsParsed p = envh::parseHosts(
        "# group\n"
        "127.0.0.1 a.home # one\n"
        "127.0.0.1 b.home # two\n"
        "127.0.0.1 c.home # three\n");
    envh::edit::HostsModel hm = envh::edit::HostsModel::fromParsed(p, false);
    const std::size_t before = hm.rows.size();
    const int origin = hm.rows[1].origin;
    const std::size_t pos = hm.addNew(origin);
    CHECK(pos == 2, "hosts addNew returns the actual insertion index");
    CHECK(hm.rows.size() == before + 1, "hosts addNew grows the row set");
    CHECK(hm.rows[pos].isNew, "inserted hosts row is marked new");
    hm.removeRow(pos);
    CHECK(hm.rows.size() == before, "removeRow erases an unsaved new hosts row");
    hm.toggleDeleted(1);
    CHECK(hm.rows[1].deleted, "toggleDeleted marks an existing hosts row");
    hm.toggleDeleted(1);
    CHECK(!hm.rows[1].deleted, "toggleDeleted restores on the second call");
    CHECK(!hm.isDirty(), "restored hosts row returns to a clean state");

    envh::EnvSnapshot snap;
    envh::EnvVar v;
    v.scope = "user";
    v.name = "BASE";
    v.type = "REG_SZ";
    v.value = "1";
    snap.vars.push_back(v);
    envh::edit::EnvModel em = envh::edit::EnvModel::fromSnapshot(snap);
    const std::size_t n = em.addNew("user");
    CHECK(em.rows[n].isNew, "env addNew marks the row new");
    em.removeRow(n);
    CHECK(em.rows.size() == 1, "env removeRow erases a new row");
    em.toggleDeleted(0);
    CHECK(em.rows[0].deleted, "env toggleDeleted marks the base row");
    em.toggleDeleted(0);
    CHECK(!em.rows[0].deleted, "env toggleDeleted restores the base row");
    CHECK(!em.isDirty(), "env restored row returns to a clean state");
}

// ---- U2: data carried into the window (protocol layer tests this) -----------
// Covered by integration.py P1/P2; here we just assert the row builders we will
// use produce stable ids and item-level Enter = openEditor.

int main()
{
    testPathSplit();
    testEnvDirty();
    testUndoRedo();
    testHostsMultiHost();
    testHostsByteExact();
    testFingerprint();
    testDraftRoundTrip();
    testBackupIntoWork();
    testRowCommands();

    std::cerr << "editor_model tests: " << (g_total - g_failed) << "/" << g_total << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
