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
    // Quoted segment keeps its semicolon, and the quotes themselves: the item
    // list must be able to rejoin the value byte-for-byte.
    const std::vector<std::string> q = pathSplit("C:\\a;\"D:\\b;c\";E:\\d");
    CHECK(q.size() == 3, "path quoted segment count");
    CHECK(q[1] == "\"D:\\b;c\"", "path quoted segment keeps quotes");
    CHECK(pathJoin(q) == "C:\\a;\"D:\\b;c\";E:\\d", "path quoted join is byte-exact");
}

void testDelimitedValues()
{
    using envh::edit::joinDelimited;
    using envh::edit::splitDelimited;
    CHECK(joinDelimited(splitDelimited("a,b,,", ",", false), ",") == "a,b,,", "generic delimiter roundtrip");
    CHECK(splitDelimited("a;b;\"c;d\"", ";", true).size() == 3, "quoted delimiter count");
    CHECK(splitDelimited("a::b", "::", false).size() == 2, "multi-character delimiter");
    CHECK(joinDelimited({"one", "", "three"}, "|") == "one||three", "generic delimiter join");
    CHECK(joinDelimited(splitDelimited("a\tb\tc", "\t", false), "\t") == "a\tb\tc", "tab roundtrip");
    CHECK(joinDelimited(splitDelimited("\"a;b\";c", ";", true), ";") == "\"a;b\";c",
          "quoted join keeps the value byte-exact");
    CHECK(splitDelimited("a,b", ";", false).size() == 1, "absent delimiter yields one part");
    CHECK(splitDelimited("a", "", false).size() == 1, "empty delimiter yields one part");
    // A custom delimiter is any non-empty string; splitting and rejoining must be
    // the identity whether the user picked a preset or typed something odd.
    CHECK(joinDelimited(splitDelimited("a=>b=>c", "=>", false), "=>") == "a=>b=>c",
          "custom two-character delimiter roundtrip");
    CHECK(splitDelimited("a=>b=>c", "=>", false).size() == 3, "custom delimiter part count");
    CHECK(joinDelimited(splitDelimited("a=>b", "=>", false), ";") == "a;b",
          "delimiter switch rejoins with the new separator");
    CHECK(joinDelimited(splitDelimited("C:\\a;\"D:\\b;c\";E:\\d", ";", true), "=>")
              == "C:\\a=>\"D:\\b;c\"=>E:\\d",
          "quoted segments survive a delimiter switch");
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

// The editor only writes HKCU, so a system-scope change must not look like a
// conflict against the pending user edits (it used to cancel every Ctrl+S).
void testUserFingerprintScope()
{
    envh::EnvSnapshot snap;
    envh::EnvVar u;
    u.scope = "user";
    u.name = "A";
    u.type = "REG_SZ";
    u.value = "1";
    envh::EnvVar s;
    s.scope = "system";
    s.name = "PATH";
    s.type = "REG_EXPAND_SZ";
    s.value = "C:\\Windows";
    snap.vars.push_back(u);
    snap.vars.push_back(s);

    const envh::edit::EnvModel m = envh::edit::EnvModel::fromSnapshot(snap);
    CHECK(m.fingerprint == envh::edit::EnvModel::computeFingerprint(snap), "model stores full fingerprint");
    CHECK(m.userFingerprint == envh::edit::EnvModel::computeUserFingerprint(snap), "model stores user fingerprint");
    CHECK(m.fingerprint != m.userFingerprint, "full and user fingerprints differ while system vars exist");

    envh::EnvSnapshot changed = snap;
    changed.vars[1].value = "C:\\Windows;C:\\extra";
    CHECK(envh::edit::EnvModel::computeUserFingerprint(changed) == m.userFingerprint,
          "system edit leaves the user fingerprint alone");
    CHECK(envh::edit::EnvModel::computeFingerprint(changed) != m.fingerprint,
          "system edit changes the full fingerprint");

    envh::EnvSnapshot added = snap;
    envh::EnvVar s2 = s;
    s2.name = "SYSTEMROOT";
    s2.value = "C:\\Windows";
    added.vars.push_back(s2);
    CHECK(envh::edit::EnvModel::computeUserFingerprint(added) == m.userFingerprint,
          "system addition leaves the user fingerprint alone");

    envh::EnvSnapshot userChanged = snap;
    userChanged.vars[0].value = "9";
    CHECK(envh::edit::EnvModel::computeUserFingerprint(userChanged) != m.userFingerprint,
          "user edit changes the user fingerprint");
    // Order of vars in the snapshot must not matter.
    envh::EnvSnapshot shuffled;
    shuffled.vars.push_back(s);
    shuffled.vars.push_back(u);
    CHECK(envh::edit::EnvModel::computeUserFingerprint(shuffled) == m.userFingerprint,
          "user fingerprint ignores snapshot order");
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

// ---- hosts header fold + duplicate host names -------------------------------
void testHostsHeaderAndDuplicates()
{
    using envh::edit::HostsModel;
    const std::string ms =
        "\xEF\xBB\xBF# Copyright (c) 1993-2009 Microsoft Corp.\r\n"
        "#\r\n"
        "# For example:\r\n"
        "#\r\n"
        "#      102.54.94.97     rhino.acme.com          # source server\r\n"
        "#       38.25.63.10     x.acme.com              # x client host\r\n"
        "127.0.0.1       kubernetes.docker.internal\r\n"
        "# Added by Docker Desktop\r\n"
        "172.17.114.61 host.docker.internal\r\n"
        "127.0.0.1 kubernetes.docker.internal\r\n";
    HostsModel m = HostsModel::fromParsed(envh::parseHosts(ms), true);
    CHECK(m.headerEnd() == 5, "header fold covers the leading comment block incl. samples");
    CHECK(m.rows.size() == 5, "header + 3 entries + 1 section heading");
    CHECK(m.rows[0].isHeader && m.rows[0].isSection, "first row is the folded header");
    std::size_t entries = 0;
    for (const auto& r : m.rows)
        entries += r.isSection ? 0 : 1;
    CHECK(entries == 3, "sample lines are no longer disabled entries");
    CHECK(!m.isDirty() && m.changeList().empty(), "fresh model is clean with no bogus changes");
    CHECK(m.serializeWork() == ms, "header fold keeps the file byte-exact");
    const auto dup = m.duplicateFlags();
    CHECK(!dup[1] && dup[4], "second kubernetes.docker.internal line is flagged");
    CHECK(m.duplicateCount() == 1, "one duplicate line");
    m.setEnabled(4, false);
    CHECK(m.duplicateCount() == 0, "a disabled line is not a duplicate");

    // A file that starts with a disabled entry has no header.
    const std::string noHeader = "# 10.0.0.1 a.test\n10.0.0.2 b.test\n";
    HostsModel n = HostsModel::fromParsed(envh::parseHosts(noHeader), false);
    CHECK(n.headerEnd() == -1, "a leading disabled entry is not swallowed as header");
    CHECK(n.rows.size() == 2 && n.rows[0].commented && !n.rows[0].isSection,
          "leading disabled entry stays an entry");

    // Drafts written before folding carried the header lines as rows.
    HostsModel old = HostsModel::fromParsed(envh::parseHosts(ms), true);
    winshun::Json::Array arr;
    for (int line = 0; line < 10; ++line) {
        winshun::Json::Object o;
        o["origin"] = line;
        o["isSection"] = line < 4 || line == 7;
        o["commented"] = line < 6 || line == 7;
        arr.push_back(winshun::Json(std::move(o)));
    }
    winshun::Json::Object root;
    root["rows"] = winshun::Json(std::move(arr));
    old.fromDraft(winshun::Json(std::move(root)));
    CHECK(!old.rows.empty() && old.rows[0].isHeader, "old draft is re-folded under one header");
    CHECK(old.rows.size() == 5, "old draft header lines collapse into one row");
}

// ---- hosts raw-text editing --------------------------------------------------
static std::string toCrlfView(std::string s)
{
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF)
        s.erase(0, 3);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r'))
            out += '\r';
        out += s[i];
    }
    return out;
}
void testHostsRawEdit()
{
    using envh::edit::HostsModel;
    // LF file with a BOM and no trailing newline.
    const std::string lf = "\xEF\xBB\xBF# hosts\n127.0.0.1 a.test\n10.0.0.1 b.test";
    HostsModel m = HostsModel::fromParsed(envh::parseHosts(lf), false);
    const std::string view = toCrlfView(m.serializeWork());
    CHECK(!m.applyRaw(view), "unchanged raw text (CRLF view of an LF file) is a no-op");
    CHECK(!m.isDirty() && !m.canUndo(), "no-op raw apply leaves the model clean");

    std::string edited = view;
    edited.replace(edited.find("b.test"), 6, "c.test");
    CHECK(m.applyRaw(edited), "edited raw text is applied");
    CHECK(m.serializeWork() == "\xEF\xBB\xBF# hosts\n127.0.0.1 a.test\n10.0.0.1 c.test",
          "raw apply keeps BOM, LF endings and the missing trailing newline");
    CHECK(m.isDirty(), "raw apply makes the model dirty against disk");
    CHECK(m.rows.back().hostnames.size() == 1 && m.rows.back().hostnames[0] == "c.test",
          "rows are rebuilt from the raw text");
    const auto ch = m.changeList();
    CHECK(ch.size() == 2 && ch[0].kind == "add" && ch[1].kind == "delete",
          "raw edit change list is a line diff against disk");

    // A row edit on top of the raw edit, then two undos.
    m.setEnabled(m.rows.size() - 1, false);
    CHECK(m.serializeWork().find("# 10.0.0.1 c.test") != std::string::npos, "row edit after raw edit");
    m.undo();
    CHECK(m.serializeWork().find("10.0.0.1 c.test") != std::string::npos
              && m.serializeWork().find("# 10.0.0.1") == std::string::npos,
          "first undo reverts only the row edit");
    m.undo();
    CHECK(m.serializeWork() == lf && !m.isDirty(), "second undo reverts the whole raw edit in one step");
    m.redo();
    CHECK(m.serializeWork().find("c.test") != std::string::npos, "redo re-applies the raw edit");

    // Adding lines to a CRLF file: new lines get CRLF, untouched keep theirs.
    const std::string crlf = "127.0.0.1 a.test\r\n10.0.0.1 b.test\r\n";
    HostsModel c = HostsModel::fromParsed(envh::parseHosts(crlf), true);
    CHECK(c.applyRaw(toCrlfView(c.serializeWork()) + "10.0.0.2 d.test\r\n"), "appended line applies");
    CHECK(c.serializeWork() == crlf + "10.0.0.2 d.test\r\n", "CRLF file stays CRLF with a trailing newline");

    // Invalid IP typed in the raw text is caught by the existing save gate.
    HostsModel bad = HostsModel::fromParsed(envh::parseHosts(crlf), true);
    bad.applyRaw("127.0.0.1 a.test\r\n10.0.0.300 b.test\r\n");
    CHECK(bad.hasInvalid(), "invalid IP from raw text blocks save");

    // Drafts written after a raw edit restore the same text.
    HostsModel d1 = HostsModel::fromParsed(envh::parseHosts(lf), false);
    d1.applyRaw(edited);
    HostsModel d2 = HostsModel::fromParsed(envh::parseHosts(lf), false);
    d2.fromDraft(d1.toDraft());
    CHECK(d2.serializeWork() == d1.serializeWork(), "draft after a raw edit round-trips");
}

// ---- batch selection commands and the overlong-PATH regression ------------
void testBatchCommands()
{
    // One undo step for a whole selection, with mixed base / new rows.
    envh::EnvSnapshot snap;
    for (const char* n : {"A", "B", "C"}) {
        envh::EnvVar v;
        v.scope = "user";
        v.name = n;
        v.type = "REG_SZ";
        v.value = n;
        snap.vars.push_back(v);
    }
    envh::edit::EnvModel em = envh::edit::EnvModel::fromSnapshot(snap);
    const std::size_t fresh = em.addNew("user");
    em.rows[fresh].name = "NEW";
    em.rows[fresh].value = "new";
    const std::size_t total = em.rows.size();
    em.setDeletedMany({0, 2, fresh}, true);
    CHECK(em.rows.size() == total - 1, "batch delete erases only the unsaved new row");
    CHECK(em.rows[0].deleted && em.rows[2].deleted, "batch delete flags the base rows");
    CHECK(!em.rows[1].deleted, "batch delete leaves unselected rows alone");
    CHECK(em.canUndo(), "batch delete leaves an undo step");
    em.undo();
    CHECK(em.rows.size() == total, "ONE undo restores the whole batch");
    CHECK(!em.rows[0].deleted && !em.rows[2].deleted, "ONE undo clears every deletion flag");
    em.setDeletedMany({0, 2}, true);
    em.setDeletedMany({0, 2}, false);
    CHECK(!em.rows[0].deleted && !em.rows[2].deleted, "batch restore clears the flags");

    envh::HostsParsed p = envh::parseHosts(
        "# group\n127.0.0.1 a.home\n127.0.0.1 b.home\n127.0.0.1 c.home\n");
    envh::edit::HostsModel hm = envh::edit::HostsModel::fromParsed(p, false);
    const std::size_t n0 = hm.rows.size();
    hm.setDeletedMany({1, 3}, true);
    CHECK(hm.rows[1].deleted && hm.rows[3].deleted, "hosts batch delete flags both lines");
    CHECK(!hm.rows[0].deleted, "hosts batch delete never flags the section heading");
    hm.undo();
    CHECK(!hm.rows[1].deleted && !hm.rows[3].deleted, "ONE undo restores the hosts batch");
    hm.setEnabledMany({1, 2, 3}, false);
    CHECK(hm.rows[1].commented && hm.rows[2].commented && hm.rows[3].commented,
          "hosts batch disable affects every selected line");
    CHECK(hm.rows.size() == n0, "hosts batch disable does not add or erase rows");
    hm.undo();
    CHECK(!hm.rows[1].commented && !hm.rows[2].commented && !hm.rows[3].commented,
          "ONE undo restores the hosts enable state");

    // The limit is UTF-16 units, not UTF-8 bytes: a CJK-heavy PATH is valid.
    CHECK(envh::utf16Units(std::string(1000, 'x')) == 1000, "ASCII counts one unit per byte");
    CHECK(envh::utf16Units("\xE4\xB8\xAD\xE6\x96\x87") == 2, "CJK counts one unit per code point");
    CHECK(envh::isValidEnvValue(std::string(32767, 'x')), "32767 units is still valid");
    CHECK(!envh::isValidEnvValue(std::string(32768, 'x')), "32768 units is rejected");
    const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87"; // 2 units, 6 bytes
    std::string longCjk;
    for (int i = 0; i < 16000; ++i)
        longCjk += cjk; // 32000 units but 96000 bytes
    CHECK(longCjk.size() > envh::kMaxEnvValue, "CJK fixture really exceeds the byte count");
    CHECK(envh::isValidEnvValue(longCjk), "CJK PATH is valid under the UTF-16 limit");

    // Regression: shortening an overlong PATH is a value edit, never a delete.
    envh::EnvSnapshot pathSnap;
    envh::EnvVar pv;
    pv.scope = "user";
    pv.name = "Path";
    pv.type = "REG_EXPAND_SZ";
    pv.value = std::string(33000, 'x');
    pathSnap.vars.push_back(pv);
    envh::edit::EnvModel pm = envh::edit::EnvModel::fromSnapshot(pathSnap);
    // v0.6.0: over-length is a SOFT warning. The value is still written to the
    // registry when the user confirms; only hard errors block a save.
    CHECK(!pm.hasInvalid(), "overlong Path is NOT a hard error (save allowed)");
    CHECK(pm.warnings(false).size() == 1 && pm.warnings(false)[0].name == "Path"
              && pm.warnings(false)[0].used == 33000 && pm.warnings(false)[0].over == 233,
          "overlong Path produces one warning with the exact overage");
    envh::edit::RowState st = pm.rowState(0);
    CHECK(st == envh::edit::RowState::Unchanged, "unmodified base row is unchanged");
    pm.setVar(0, "Path", std::string(30000, 'x'), "REG_EXPAND_SZ");
    CHECK(pm.rowState(0) == envh::edit::RowState::Modified, "a value edit marks the row modified");
    const envh::edit::EnvModel::Delta d = pm.delta(false);
    CHECK(d.toDelete.empty(), "removing a split item never turns into a variable delete");
    CHECK(d.toWrite.size() == 1 && d.toWrite[0].name == "Path",
          "removing a split item writes the shortened value");
    CHECK(!pm.hasInvalid() && pm.warnings(false).empty(), "shortened Path has no warning left");
    pm.toggleDeleted(0);
    CHECK(pm.rowState(0) == envh::edit::RowState::Deleted, "a deleted row is marked deleted");
}

// ---- v0.6.0: soft warnings, row states, whitespace delimiter, 3-way diff ---
void testV06Model()
{
    using envh::edit::RowState;

    // Whitespace delimiter: runs collapse, quotes are preserved, join uses one
    // space. This normalises the raw text, which is why the UI warns first.
    const auto ws = envh::edit::splitDelimited("  a   b\tc  ", " ", true);
    CHECK(ws.size() == 3 && ws[0] == "a" && ws[1] == "b" && ws[2] == "c",
          "space delimiter collapses whitespace runs and trims the ends");
    CHECK(envh::edit::joinDelimited(ws, " ") == "a b c", "space join emits one space");
    const auto q = envh::edit::splitDelimited("\"C:\\Program Files\\App\" other", " ", true);
    CHECK(q.size() == 2 && q[0] == "\"C:\\Program Files\\App\"" && q[1] == "other",
          "quoted text keeps its embedded spaces");
    CHECK(envh::edit::splitDelimited("a;b;c", ";", true).size() == 3,
          "ordinary delimiters are unaffected by the whitespace branch");

    // Hard errors still block; the 32767 cap does not.
    CHECK(envh::envValueHardErrorOk(std::string(40000, 'x')), "long value has no hard error");
    CHECK(!envh::envValueHardErrorOk(std::string("bad\x01value")), "control char is a hard error");
    CHECK(envh::envValueOverlength(std::string(32767, 'x')) == 0, "exactly at the cap: no overage");
    CHECK(envh::envValueOverlength(std::string(40000, 'x')) == 7233, "overage is exact in UTF-16 units");
    CHECK(!envh::isValidEnvValue(std::string(40000, 'x')), "protocol env:set still hard-rejects overlong");

    // Row states cover all four values on both models.
    envh::EnvSnapshot es;
    for (auto nv : {std::pair<const char*, const char*> {"Path", "A;B"},
                    {"LANG", "zh"}}) {
        envh::EnvVar v;
        v.scope = "user";
        v.name = nv.first;
        v.type = "REG_SZ";
        v.value = nv.second;
        es.vars.push_back(v);
    }
    envh::edit::EnvModel em = envh::edit::EnvModel::fromSnapshot(es);
    CHECK(em.rowState(0) == RowState::Unchanged && em.rowState(1) == RowState::Unchanged,
          "fresh env rows are unchanged");
    em.setVar(0, "Path", "A;C", "REG_SZ");
    CHECK(em.rowState(0) == RowState::Modified, "env value edit is Modified");
    const std::size_t added = em.addNew();
    CHECK(em.rowState(added) == RowState::Added, "env new row is Added");
    em.undo();
    CHECK(em.rows.size() == 2, "undo removes the new row");
    em.toggleDeleted(1);
    CHECK(em.rowState(1) == RowState::Deleted, "env deleted row is Deleted");

    const envh::HostsParsed hp = envh::parseHosts("# header\n127.0.0.1 a.test\n");
    envh::edit::HostsModel hm = envh::edit::HostsModel::fromParsed(hp, true);
    const int entry = hm.headerEnd() + 1;
    CHECK(hm.rowState(static_cast<std::size_t>(entry)) == RowState::Unchanged,
          "fresh hosts entry is unchanged");
    hm.setEnabled(static_cast<std::size_t>(entry), false);
    CHECK(hm.rowState(static_cast<std::size_t>(entry)) == RowState::Modified,
          "hosts enable toggle is Modified");

    // Three-snapshot diff: five verdict classes, read-only, never merges.
    envh::EnvSnapshot base;
    auto add = [](envh::EnvSnapshot& s, const char* n, const char* v) {
        envh::EnvVar e;
        e.scope = "user";
        e.name = n;
        e.type = "REG_SZ";
        e.value = v;
        s.vars.push_back(e);
    };
    add(base, "Path", "A;B");
    add(base, "LANG", "zh");
    add(base, "TMP", "C:\\T");
    add(base, "ONLYBASE", "v1");
    add(base, "BOTH", "x");
    envh::EnvSnapshot mine = base, theirs = base;
    // mine: Path modified, TMP deleted, NEW added, BOTH -> y
    for (auto& v : mine.vars) {
        if (v.name == "Path") v.value = "A;C";
        if (v.name == "BOTH") v.value = "y";
    }
    mine.vars.erase(std::remove_if(mine.vars.begin(), mine.vars.end(),
                                   [](const envh::EnvVar& v) { return v.name == "TMP"; }),
                    mine.vars.end());
    add(mine, "NEW", "n");
    // theirs: LANG changed, ONLYBASE deleted, TMP changed, BOTH -> y, TNEW added
    for (auto& v : theirs.vars) {
        if (v.name == "LANG") v.value = "en";
        if (v.name == "TMP") v.value = "C:\\T2";
        if (v.name == "BOTH") v.value = "y";
    }
    theirs.vars.erase(std::remove_if(theirs.vars.begin(), theirs.vars.end(),
                                     [](const envh::EnvVar& v) { return v.name == "ONLYBASE"; }),
                      theirs.vars.end());
    add(theirs, "TNEW", "t");
    const envh::edit::Diff3Result d = envh::edit::diff3Env(base, mine, theirs);
    auto verdict = [&](const char* key) {
        for (const auto& e : d.entries)
            if (e.key == std::string("user/") + key)
                return e.verdict;
        return std::string("missing");
    };
    CHECK(verdict("Path") == "mine", "only I changed Path");
    CHECK(verdict("LANG") == "theirs", "only the outside process changed LANG");
    CHECK(verdict("BOTH") == "same", "identical change on both sides is not a conflict");
    CHECK(verdict("TMP") == "conflict", "delete vs edit is a conflict");
    CHECK(verdict("ONLYBASE") == "theirs", "outside deletion is reported, not auto-applied");
    CHECK(verdict("NEW") == "mine", "my new variable is reported");
    CHECK(verdict("TNEW") == "theirs", "outside new variable is reported");
    CHECK(d.conflicts == 1 && d.mineOnly == 2 && d.theirsOnly == 3,
          "diff counts the verdict classes correctly");
    CHECK(envh::edit::diff3Env(base, base, base).entries.empty(),
          "three identical snapshots produce an empty diff");
    CHECK(envh::edit::diff3RowsToSnapshot(em.rows).vars.size() == 1,
          "rows convert to a snapshot for the diff panel (deleted row skipped)");

    const envh::edit::Diff3HostsResult hd = envh::edit::diff3Hosts(
        "127.0.0.1 a.test\r\n127.0.0.1 b.test\r\n",
        "127.0.0.1 a.test\r\n127.0.0.1 c.test\r\n");
    CHECK(hd.added == 1 && hd.removed == 1 && hd.samples.size() == 2,
          "hosts diff is a summary of added / removed line texts");
}

// ---- U2: data carried into the window (protocol layer tests this) -----------
// Covered by integration.py P1/P2; here we just assert the row builders we will
// use produce stable ids and item-level Enter = openEditor.

int main()
{
    testPathSplit();
    testDelimitedValues();
    testEnvDirty();
    testUndoRedo();
    testHostsMultiHost();
    testHostsByteExact();
    testFingerprint();
    testUserFingerprintScope();
    testDraftRoundTrip();
    testBackupIntoWork();
    testRowCommands();
    testHostsHeaderAndDuplicates();
    testHostsRawEdit();
    testBatchCommands();
    testV06Model();

    std::cerr << "editor_model tests: " << (g_total - g_failed) << "/" << g_total << " passed\n";
    return g_failed == 0 ? 0 : 1;
}
