// editor_model.h - the editable working copy behind the interactive editor
// windows (env and hosts), with no Win32 dependency.
//
// This is the same idea as logic.h: pure, deterministic, unit-testable. The
// window (editor_win.h) and main.cpp hold the OS state; this file only models
// "what the user has on screen" and "what would a save do". Keeping it OS-free
// means the dirty tracking, undo/redo, PATH split/join, multi-hostname hosts
// lines, fingerprint and draft round-trip are all covered by tests that run
// without the WinShun host or the registry.
//
// Code and comments are English.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "logic.h"
#include "winshun.h"

namespace envh {
namespace edit {

// How a working row compares with the snapshot the window opened with. The
// editor paints this per row so "unsaved change" is visible on the item that
// actually changed, not just in a global status line.
enum class RowState { Unchanged, Added, Modified, Deleted };

// A value that Windows will let us write to the registry but that a new
// process may not be able to read back in full. Soft warning, not a hard error.
struct EnvWarning {
    std::string name;
    std::size_t used = 0;
    std::size_t cap = 0;
    std::size_t over = 0;
};

// ---------------------------------------------------------------------------
// PATH helpers (env list-mode editing)
// ---------------------------------------------------------------------------

inline std::vector<std::string> splitDelimited(const std::string& value, const std::string& delimiter, bool preserveQuoted)
{
    if (delimiter.empty())
        return {value};
    // " " stands for ANY whitespace run. joinDelimited() then emits one single
    // space, so switching to this delimiter normalises the original spacing.
    // The window warns before applying it (and refuses it for PATH by default).
    if (delimiter == " ") {
        std::vector<std::string> parts;
        std::string current;
        bool inQuote = false;
        auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        for (std::size_t i = 0; i < value.size();) {
            const char c = value[i];
            if (preserveQuoted && c == '"') {
                inQuote = !inQuote;
                current += c;
                ++i;
                continue;
            }
            if (!inQuote && ws(c)) {
                parts.push_back(current);
                current.clear();
                while (i < value.size() && ws(value[i]))
                    ++i;
                continue;
            }
            current += c;
            ++i;
        }
        parts.push_back(current);
        // Leading / trailing whitespace in the raw value is a separator, not an
        // empty item. Keep interior items intact (quoted text can be empty).
        if (parts.size() > 1 && parts.front().empty())
            parts.erase(parts.begin());
        if (parts.size() > 1 && parts.back().empty())
            parts.pop_back();
        if (parts.empty())
            parts.emplace_back();
        return parts;
    }
    std::vector<std::string> parts;
    std::string current;
    bool inQuote = false;
    for (std::size_t i = 0; i < value.size();) {
        if (preserveQuoted && value[i] == '"') {
            inQuote = !inQuote;
            current += value[i++];
            continue;
        }
        if (!inQuote && value.compare(i, delimiter.size(), delimiter) == 0) {
            parts.push_back(current);
            current.clear();
            i += delimiter.size();
            continue;
        }
        current += value[i++];
    }
    parts.push_back(current);
    return parts;
}

inline std::string joinDelimited(const std::vector<std::string>& parts, const std::string& delimiter)
{
    std::string value;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            value += delimiter;
        value += parts[i];
    }
    return value;
}

inline std::vector<std::string> pathSplit(const std::string& v)
{
    return splitDelimited(v, ";", true);
}

inline std::string pathJoin(const std::vector<std::string>& segs)
{
    return joinDelimited(segs, ";");
}

// ---------------------------------------------------------------------------
// ENV model
// ---------------------------------------------------------------------------

struct EnvVarRow {
    std::string scope; // "user" | "system"
    std::string name;
    std::string type; // "REG_SZ" | "REG_EXPAND_SZ" | "REG_MULTI_SZ"
    std::string value;
    bool inBase = false; // present in the committed snapshot
    bool isNew = false; // added in the window, not in base
    bool deleted = false; // marked for deletion (kept visible, struck through)
};

struct EnvChange {
    std::string kind; // "add" | "delete" | "modify" | "type"
    std::string name;
    std::string detail; // human-readable line for the save dialog
};

struct EnvModel {
    std::vector<EnvVarRow> base; // committed snapshot (fingerprint source)
    std::vector<EnvVarRow> rows; // working copy shown in the window
    std::string fingerprint; // FNV-1a over every scope in base
    std::string userFingerprint; // FNV-1a over user-scope variables only

    static EnvModel fromSnapshot(const EnvSnapshot& snap)
    {
        EnvModel m;
        for (const EnvVar& v : snap.vars) {
            EnvVarRow r;
            r.scope = v.scope;
            r.name = v.name;
            r.type = v.type;
            r.value = v.value;
            r.inBase = true;
            m.base.push_back(r);
            m.rows.push_back(r);
        }
        m.fingerprint = computeFingerprint(snap);
        m.userFingerprint = computeUserFingerprint(snap);
        return m;
    }

    // FNV-1a over sorted "scope\nname\n type\nvalue\n" of the snapshot.
    static std::string computeFingerprint(const EnvSnapshot& snap)
    {
        return fingerprintSnapshot(snap, false);
    }

    // The editor can only write HKCU values. Changes to HKLM therefore must not
    // invalidate a pending user edit or make every Ctrl+S look like a conflict.
    static std::string computeUserFingerprint(const EnvSnapshot& snap)
    {
        return fingerprintSnapshot(snap, true);
    }

private:
    static std::string fingerprintSnapshot(const EnvSnapshot& snap, bool userOnly)
    {
        std::vector<std::string> lines;
        for (const EnvVar& v : snap.vars) {
            if (userOnly && v.scope != "user")
                continue;
            lines.push_back(v.scope + "\n" + toLower(v.name) + "\n" + v.type + "\n" + v.value + "\n");
        }
        std::sort(lines.begin(), lines.end());
        std::string blob;
        for (const std::string& l : lines)
            blob += l;
        return fnv1aHex(blob);
    }

public:

    static bool varEqual(const EnvVarRow& a, const EnvVarRow& b)
    {
        return a.scope == b.scope && toLower(a.name) == toLower(b.name)
               && a.type == b.type && a.value == b.value && !a.deleted && !b.deleted;
    }

    bool isDirty() const
    {
        // A base var that no live work row matches is a deletion.
        for (const EnvVarRow& b : base) {
            bool matched = false;
            for (const EnvVarRow& r : rows) {
                if (toLower(r.name) == toLower(b.name) && r.scope == b.scope) {
                    if (r.deleted || !varEqual(r, b))
                        return true; // deleted or modified
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return true; // base var gone
        }
        // A work row not in base is an addition.
        for (const EnvVarRow& r : rows) {
            if (r.isNew)
                return true;
            bool inBase = false;
            for (const EnvVarRow& b : base)
                if (toLower(r.name) == toLower(b.name) && r.scope == b.scope)
                    inBase = true;
            if (!inBase)
                return true;
        }
        return false;
    }

    std::vector<EnvChange> changeList() const
    {
        std::vector<EnvChange> out;
        // Modifications and deletions over base vars that exist.
        for (const EnvVarRow& b : base) {
            const EnvVarRow* live = nullptr;
            for (const EnvVarRow& r : rows)
                if (toLower(r.name) == toLower(b.name) && r.scope == b.scope) {
                    live = &r;
                    break;
                }
            if (!live) {
                EnvChange c;
                c.kind = "delete";
                c.name = b.name;
                c.detail = "- " + b.name;
                out.push_back(c);
            } else if (live->deleted) {
                EnvChange c;
                c.kind = "delete";
                c.name = b.name;
                c.detail = "- " + b.name;
                out.push_back(c);
            } else if (live->type != b.type) {
                EnvChange c;
                c.kind = "type";
                c.name = b.name;
                c.detail = "~ " + b.name + ": " + b.type + " -> " + live->type;
                out.push_back(c);
            } else if (live->value != b.value) {
                EnvChange c;
                c.kind = "modify";
                c.name = b.name;
                c.detail = "~ " + b.name + ": " + truncate(b.value, 40) + " (was " + truncate(b.value, 40) + ")";
                out.push_back(c);
            }
        }
        // Additions.
        for (const EnvVarRow& r : rows) {
            if (!r.isNew)
                continue;
            if (r.name.empty())
                continue; // skip an empty in-progress row
            EnvChange c;
            c.kind = "add";
            c.name = r.name;
            c.detail = "+ " + r.name + " = " + truncate(r.value, 40);
            out.push_back(c);
        }
        return out;
    }

    // Per-row dirty state for the table paint. `base` is the snapshot taken
    // when the window opened; `rows` is the live working copy.
    RowState rowState(std::size_t idx) const
    {
        if (idx >= rows.size())
            return RowState::Unchanged;
        const EnvVarRow& r = rows[idx];
        if (r.deleted)
            return RowState::Deleted;
        if (!r.inBase || r.isNew)
            return RowState::Added;
        for (const EnvVarRow& b : base)
            if (b.scope == r.scope && toLower(b.name) == toLower(r.name))
                return (b.name != r.name || b.type != r.type || b.value != r.value)
                           ? RowState::Modified
                           : RowState::Unchanged;
        return RowState::Added;
    }

    std::vector<EnvWarning> warnings(bool systemWritable = false) const
    {
        std::vector<EnvWarning> out;
        for (const EnvVarRow& r : rows) {
            if (r.deleted || (r.scope == "system" && !systemWritable))
                continue;
            const std::size_t used = utf16Units(r.value);
            if (used > kMaxEnvValue)
                out.push_back({r.name, used, kMaxEnvValue, used - kMaxEnvValue});
        }
        return out;
    }

    // Validation: only HARD errors block a save. An over-long value is a soft
    // warning since v0.6.0 (the user explicitly chose "save anyway"); the
    // registry accepts it, and the editor shows four visible warning layers.
    // System rows only count when this window can write them (elevated plugin).
    bool hasInvalid(bool systemWritable = false) const
    {
        for (const EnvVarRow& r : rows) {
            if (r.deleted || (r.scope == "system" && !systemWritable))
                continue;
            if (r.name.empty() || !isValidEnvName(r.name))
                return true;
            if (!envValueHardErrorOk(r.value))
                return true;
        }
        return false;
    }

    // Produce the delta a save would apply. User scope always; system scope
    // only when `systemWritable` (otherwise its rows are read-only anyway).
    struct Delete {
        std::string scope;
        std::string name;
    };
    struct Delta {
        std::vector<EnvVarRow> toWrite; // each row carries its own scope
        std::vector<Delete> toDelete;
    };
    Delta delta(bool systemWritable = false) const
    {
        Delta d;
        for (const EnvVarRow& r : rows) {
            if (r.scope != "user" && !(systemWritable && r.scope == "system"))
                continue;
            if (r.deleted) {
                if (!r.isNew)
                    d.toDelete.push_back({r.scope, r.name});
                continue;
            }
            const EnvVarRow* bptr = nullptr;
            for (const EnvVarRow& b : base)
                if (toLower(r.name) == toLower(b.name) && b.scope == r.scope) {
                    bptr = &b;
                    break;
                }
            if (!bptr || r.isNew || bptr->value != r.value || bptr->type != r.type)
                d.toWrite.push_back(r);
        }
        return d;
    }

    // --- mutations (each pushes an undo snapshot) ---

    void pushUndo()
    {
        m_undo.push_back(clone(rows));
        if (m_undo.size() > 200)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }

    void undo()
    {
        if (m_undo.empty())
            return;
        m_redo.push_back(clone(rows));
        rows = m_undo.back();
        m_undo.pop_back();
    }

    void redo()
    {
        if (m_redo.empty())
            return;
        m_undo.push_back(clone(rows));
        rows = m_redo.back();
        m_redo.pop_back();
    }

    void setVar(std::size_t idx, const std::string& name, const std::string& value, const std::string& type)
    {
        if (idx >= rows.size())
            return;
        pushUndo();
        rows[idx].name = name;
        rows[idx].value = value;
        rows[idx].type = type;
    }

    void toggleDeleted(std::size_t idx)
    {
        if (idx >= rows.size())
            return;
        pushUndo();
        rows[idx].deleted = !rows[idx].deleted;
        // un-deleting a name that exists in base is just clearing the flag
        if (!rows[idx].deleted)
            rows[idx].isNew = false;
    }

    // One undo step for a whole selection. Base rows get the deletion flag;
    // rows created in this window are erased instead (descending, so the
    // remaining indices stay valid). Indices all refer to the pre-call rows.
    void setDeletedMany(const std::vector<std::size_t>& idx, bool deleted)
    {
        if (idx.empty())
            return;
        pushUndo();
        for (std::size_t i : idx)
            if (i < rows.size() && rows[i].inBase) {
                rows[i].deleted = deleted;
                if (!deleted)
                    rows[i].isNew = false;
            }
        std::vector<std::size_t> fresh;
        for (std::size_t i : idx)
            if (i < rows.size() && !rows[i].inBase)
                fresh.push_back(i);
        std::sort(fresh.begin(), fresh.end(), std::greater<std::size_t>());
        for (std::size_t i : fresh)
            if (i < rows.size() && !rows[i].inBase)
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
    }

    std::size_t addNew(const std::string& scope = "user")
    {
        pushUndo();
        EnvVarRow r;
        r.scope = scope;
        r.name = "";
        r.value = "";
        r.type = "REG_SZ";
        r.inBase = false;
        r.isNew = true;
        rows.push_back(r);
        return rows.size() - 1;
    }

    void removeRow(std::size_t idx)
    {
        if (idx >= rows.size())
            return;
        pushUndo();
        if (rows[idx].inBase)
            rows[idx].deleted = true; // keep for undo; struck through
        else
            rows.erase(rows.begin() + idx);
    }

    // --- draft (saved when the host closes us with edits) ---
    winshun::Json toDraft() const
    {
        winshun::Json::Array arr;
        for (const EnvVarRow& r : rows) {
            winshun::Json::Object o;
            o["scope"] = r.scope;
            o["name"] = r.name;
            o["type"] = r.type;
            o["value"] = r.value;
            o["inBase"] = r.inBase;
            o["isNew"] = r.isNew;
            o["deleted"] = r.deleted;
            arr.push_back(winshun::Json(std::move(o)));
        }
        winshun::Json::Object root;
        root["rows"] = winshun::Json(std::move(arr));
        return winshun::Json(std::move(root));
    }

    void fromDraft(const winshun::Json& d)
    {
        rows.clear();
        m_undo.clear();
        m_redo.clear();
        for (const winshun::Json& e : d["rows"].toArray()) {
            EnvVarRow r;
            r.scope = e["scope"].toString();
            r.name = e["name"].toString();
            r.type = e["type"].toString();
            r.value = e["value"].toString();
            r.inBase = e["inBase"].toBool();
            r.isNew = e["isNew"].toBool();
            r.deleted = e["deleted"].toBool();
            rows.push_back(r);
        }
    }

private:
    std::vector<std::vector<EnvVarRow>> m_undo;
    std::vector<std::vector<EnvVarRow>> m_redo;

    static std::vector<EnvVarRow> clone(const std::vector<EnvVarRow>& in)
    {
        return in;
    }
    static std::string truncate(const std::string& s, std::size_t n)
    {
        return s.size() > n ? s.substr(0, n) + "…" : s;
    }
};

// ---------------------------------------------------------------------------
// HOSTS model
// ---------------------------------------------------------------------------

struct HostsRow {
    int origin = -1; // index into HostsParsed::lines, -1 = new row
    bool isSection = false; // comment-only divider ("# Added by Docker")
    bool isHeader = false; // the folded leading comment block (implies isSection)
    std::string sectionText; // raw divider text (kept byte-exact)
    bool commented = false; // leading '#' present
    std::string ip;
    std::vector<std::string> hostnames; // all host names on the line
    std::string comment; // trailing "# ..."
    bool deleted = false;
    bool isNew = false;
    bool crlf = false; // line ending to emit for edited/new lines
};

struct HostsChange {
    std::string kind; // "add" | "delete" | "enable" | "disable" | "modify"
    int line; // 1-based line for display
    std::string detail;
};

struct HostsModel {
    HostsParsed disk; // the file as read; dirtiness, conflicts and backups use this
    HostsParsed base; // what the rows index into (== disk until the raw text is edited)
    std::vector<HostsRow> rows; // working copy, in file order
    std::string fingerprint; // FNV over the disk content + size

    static std::string contentFingerprint(const std::string& content)
    {
        return std::to_string(content.size()) + ":" + fnv1aHex(content);
    }

    // The leading comment block of a hosts file (Microsoft's template, a
    // vendor banner) is folded into one heading row. Without this the two
    // sample lines "#  102.54.94.97  rhino.acme.com" showed up as disabled
    // entries. The block only counts as a header when it starts with prose: a
    // file whose first content line is a commented host spec has no header,
    // so a disabled first entry is never swallowed. Returns the last line
    // index of the block, or -1.
    static int findHeaderEnd(const HostsParsed& h)
    {
        int end = -1;
        bool sawProse = false;
        for (std::size_t i = 0; i < h.lines.size(); ++i) {
            const std::string& t = h.lines[i].text;
            if (trimCopy(t).empty())
                continue;
            if (!lineIsCommented(t))
                break;
            const HostsEntry e = parseHostLine(t, static_cast<int>(i));
            if (!e.section && !sawProse)
                break;
            if (e.section)
                sawProse = true;
            end = static_cast<int>(i);
        }
        return end;
    }

    static HostsModel fromParsed(const HostsParsed& h, bool dominantCrlf)
    {
        HostsModel m;
        m.disk = h;
        m.base = h;
        m.fingerprint = contentFingerprint(serializeHosts(h));
        m.m_dominantCrlf = dominantCrlf;
        m.buildRows();
        return m;
    }

    // Rebuild every row from `base`. Used on load and after the raw text has
    // been edited, which invalidates all row -> line references at once.
    void buildRows()
    {
        const HostsParsed& h = base;
        rows.clear();
        m_headerEnd = findHeaderEnd(h);
        bool headerPushed = false;
        for (std::size_t i = 0; i < h.lines.size(); ++i) {
            const HostsLine& line = h.lines[i];
            if (line.text.empty())
                continue; // blank lines kept by line index, not as rows
            const int idx = static_cast<int>(i);
            if (idx <= m_headerEnd) {
                if (!headerPushed) {
                    HostsRow r;
                    r.origin = idx;
                    r.crlf = line.crlf;
                    r.isSection = true;
                    r.isHeader = true;
                    r.sectionText = line.text;
                    rows.push_back(r);
                    headerPushed = true;
                }
                continue;
            }
            const HostsEntry e = parseHostLine(line.text, idx);
            HostsRow r;
            r.origin = idx;
            r.crlf = line.crlf;
            if (e.section) {
                r.isSection = true;
                r.sectionText = line.text;
            } else {
                r.commented = e.commented;
                r.ip = e.ip;
                r.hostnames = e.hostnames.empty() ? std::vector<std::string> {e.hostname} : e.hostnames;
                r.comment = e.comment;
            }
            rows.push_back(r);
        }
    }

    // The raw tab's text, as the user sees it (one string, any line endings).
    // It replaces `base` and every row; `disk` is untouched so dirtiness,
    // conflict checks and backups still refer to the real file. The BOM, the
    // dominant line ending and the trailing-newline state of the current text
    // are kept, so a raw edit that only changes one line saves only that line.
    // Returns false (and changes nothing) when the text is identical.
    bool applyRaw(const std::string& text)
    {
        std::string body;
        body.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                continue; // normalise CRLF -> LF; endings are re-applied below
            body += text[i];
        }
        const bool trailing = base.lines.empty() ? true : base.trailingNewline;
        if (!body.empty() && body.back() == '\n')
            body.pop_back();
        HostsParsed next;
        next.bom = base.bom;
        next.trailingNewline = trailing;
        if (!body.empty() || !base.lines.empty()) {
            std::size_t start = 0;
            for (std::size_t i = 0; i <= body.size(); ++i) {
                if (i == body.size() || body[i] == '\n') {
                    HostsLine l;
                    l.text = body.substr(start, i - start);
                    l.crlf = m_dominantCrlf;
                    next.lines.push_back(std::move(l));
                    start = i + 1;
                }
            }
        }
        // A line whose text did not change keeps its original ending, so a
        // mixed CRLF/LF file is not normalised by an unrelated raw edit.
        for (std::size_t i = 0; i < next.lines.size() && i < base.lines.size(); ++i)
            if (next.lines[i].text == base.lines[i].text)
                next.lines[i].crlf = base.lines[i].crlf;
        if (serializeHosts(next) == serializeWork())
            return false;
        pushUndo();
        base = std::move(next);
        buildRows();
        return true;
    }

    // Dirty means "saving would change the file", whichever way the edit was
    // made (row edits, raw text, or both).
    bool isDirty() const
    {
        return serializeWork() != serializeHosts(disk);
    }

    std::vector<HostsChange> changeList() const
    {
        if (!sameLines(base, disk))
            return lineDiff();
        std::vector<HostsChange> out;
        for (const HostsRow& r : rows) {
            // Headings are display-only. They used to fall through to the
            // comment-state check below and report a bogus "enable line N".
            if (r.isSection)
                continue;
            const int line = r.origin >= 0 ? r.origin + 1 : -1;
            if (r.isNew && !r.deleted) {
                HostsChange c;
                c.kind = "add";
                c.line = line;
                c.detail = std::string("+ ") + (r.commented ? "# " : "") + r.ip + " " + joinHosts(r.hostnames)
                           + (r.comment.empty() ? "" : " # " + r.comment);
                out.push_back(c);
                continue;
            }
            if (r.deleted) {
                HostsChange c;
                c.kind = "delete";
                c.line = line;
                c.detail = "- line " + std::to_string(line);
                out.push_back(c);
                continue;
            }
            const HostsEntry e = baseEntry(r.origin);
            if (e.commented != r.commented) {
                HostsChange c;
                c.kind = r.commented ? "disable" : "enable";
                c.line = line;
                c.detail = std::string(r.commented ? "disable" : "enable") + " line " + std::to_string(line);
                out.push_back(c);
            } else if (e.ip != r.ip || e.comment != r.comment
                       || (e.hostnames.empty() ? std::vector<std::string> {e.hostname} : e.hostnames) != r.hostnames) {
                HostsChange c;
                c.kind = "modify";
                c.line = line;
                c.detail = "~ line " + std::to_string(line) + " " + r.ip + " " + joinHosts(r.hostnames);
                out.push_back(c);
            }
        }
        return out;
    }

    bool hasInvalid() const
    {
        for (const HostsRow& r : rows) {
            if (r.deleted || r.isSection)
                continue;
            if (!r.commented) {
                const bool ipv4 = ipv4Valid(r.ip);
                const bool ipv6 = !ipv4 && r.ip.find(':') != std::string::npos;
                if (!ipv4 && !ipv6)
                    return true;
                for (const std::string& hn : r.hostnames)
                    if (!hostValid(hn))
                        return true;
            }
        }
        return false;
    }

    // A host name that an earlier enabled line already maps. Windows uses the
    // first match, so the later line is either redundant or silently ignored.
    std::vector<bool> duplicateFlags() const
    {
        std::vector<bool> out(rows.size(), false);
        std::map<std::string, std::size_t> seen;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const HostsRow& r = rows[i];
            if (r.isSection || r.deleted || r.commented)
                continue;
            for (const std::string& hn : r.hostnames) {
                if (hn.empty())
                    continue;
                if (!seen.emplace(toLower(hn), i).second)
                    out[i] = true;
            }
        }
        return out;
    }
    std::size_t duplicateCount() const
    {
        std::size_t n = 0;
        for (bool b : duplicateFlags())
            n += b ? 1 : 0;
        return n;
    }
    int headerEnd() const { return m_headerEnd; }

    // Re-emit the full file. Untouched lines are copied byte-for-byte so the
    // BOM, CRLF/LF mix, blank lines and comments survive. Only edited/new lines
    // are (re)serialized from the row fields.
    HostsParsed toParsed() const
    {
        HostsParsed out;
        out.bom = base.bom;
        out.trailingNewline = base.trailingNewline;
        // Map origin -> row for quick lookup.
        std::map<int, const HostsRow*> byOrigin;
        for (const HostsRow& r : rows)
            if (r.origin >= 0)
                byOrigin[r.origin] = &r;
        // New rows between original rows belong immediately after their
        // preceding original line, even when that line is marked deleted.
        // Rows appended to the working list stay after trailing blank lines.
        std::map<int, std::vector<const HostsRow*>> insertedAfter;
        int lastOriginal = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].origin >= 0)
                lastOriginal = static_cast<int>(i);
        int preceding = -1;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const HostsRow& r = rows[i];
            if (r.origin >= 0)
                preceding = r.isHeader ? m_headerEnd : r.origin;
            else if (!r.deleted)
                insertedAfter[static_cast<int>(i) > lastOriginal ? static_cast<int>(base.lines.size()) : preceding].push_back(&r);
        }
        const auto emitInserted = [&](int origin) {
            const auto it = insertedAfter.find(origin);
            if (it == insertedAfter.end())
                return;
            for (const HostsRow* r : it->second) {
                HostsLine line;
                line.text = emitRow(*r);
                line.crlf = m_dominantCrlf;
                out.lines.push_back(line);
            }
        };
        for (std::size_t i = 0; i < base.lines.size(); ++i) {
            emitInserted(static_cast<int>(i) - 1);
            if (static_cast<int>(i) <= m_headerEnd) {
                out.lines.push_back(base.lines[i]); // folded header: byte-exact
                continue;
            }
            const auto it = byOrigin.find(static_cast<int>(i));
            if (it == byOrigin.end()) {
                // No row references this line: blank lines stay, host lines are
                // considered deleted.
                if (base.lines[i].text.empty())
                    out.lines.push_back(base.lines[i]);
                continue;
            }
            const HostsRow& r = *it->second;
            if (r.deleted)
                continue; // removed line
            if (!rowChanged(r, static_cast<int>(i))) {
                out.lines.push_back(base.lines[i]); // byte-exact
            } else {
                HostsLine nl;
                nl.text = emitRow(r);
                nl.crlf = r.origin >= 0 ? base.lines[i].crlf : m_dominantCrlf;
                out.lines.push_back(nl);
            }
        }
        emitInserted(static_cast<int>(base.lines.size()) - 1);
        emitInserted(static_cast<int>(base.lines.size()));
        return out;
    }

    std::string serializeWork() const { return serializeHosts(toParsed()); }

    // --- mutations ---
    // An undo step is (base, rows, header): a raw edit replaces all three at
    // once, so restoring only the rows would point them into the wrong text.
    struct Snapshot {
        HostsParsed base;
        std::vector<HostsRow> rows;
        int headerEnd = -1;
    };
    Snapshot snapshot() const { return {base, rows, m_headerEnd}; }
    void restore(Snapshot s)
    {
        base = std::move(s.base);
        rows = std::move(s.rows);
        m_headerEnd = s.headerEnd;
    }
    void pushUndo()
    {
        m_undo.push_back(snapshot());
        if (m_undo.size() > 200)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void undo()
    {
        if (m_undo.empty())
            return;
        m_redo.push_back(snapshot());
        restore(std::move(m_undo.back()));
        m_undo.pop_back();
    }
    void redo()
    {
        if (m_redo.empty())
            return;
        m_undo.push_back(snapshot());
        restore(std::move(m_redo.back()));
        m_redo.pop_back();
    }

    void setEnabled(std::size_t idx, bool enabled)
    {
        if (idx >= rows.size() || rows[idx].isSection || rows[idx].isNew)
            return;
        pushUndo();
        rows[idx].commented = !enabled;
    }
    void setField(std::size_t idx, const std::string& ip,
                  const std::vector<std::string>& hostnames, const std::string& comment)
    {
        if (idx >= rows.size() || rows[idx].isSection)
            return;
        pushUndo();
        rows[idx].ip = ip;
        rows[idx].hostnames = hostnames;
        rows[idx].comment = comment;
    }
    std::size_t addNew(int afterLine = -1)
    {
        pushUndo();
        HostsRow r;
        r.origin = -1;
        r.commented = false;
        r.ip = "127.0.0.1";
        r.hostnames = {""};
        r.isNew = true;
        if (afterLine < 0) {
            rows.push_back(r);
            return rows.size() - 1;
        }
        // Insert after the first row whose origin >= afterLine and return the
        // ACTUAL insertion index, not the last index of the vector.
        std::size_t pos = rows.size();
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].origin >= afterLine) {
                pos = i + 1;
                break;
            }
        rows.insert(rows.begin() + pos, r);
        return pos;
    }
    void toggleDeleted(std::size_t idx)
    {
        if (idx >= rows.size() || rows[idx].isSection)
            return;
        pushUndo();
        if (rows[idx].origin >= 0)
            rows[idx].deleted = !rows[idx].deleted;
        else
            rows.erase(rows.begin() + idx);
    }

    // One undo step for a whole selection. Section headings are skipped here
    // too. New rows are erased in descending order so the remaining indices
    // still refer to the same rows.
    void setDeletedMany(const std::vector<std::size_t>& idx, bool deleted)
    {
        if (idx.empty())
            return;
        pushUndo();
        for (std::size_t i : idx)
            if (i < rows.size() && !rows[i].isSection && rows[i].origin >= 0)
                rows[i].deleted = deleted;
        std::vector<std::size_t> fresh;
        for (std::size_t i : idx)
            if (i < rows.size() && !rows[i].isSection && rows[i].origin < 0)
                fresh.push_back(i);
        std::sort(fresh.begin(), fresh.end(), std::greater<std::size_t>());
        for (std::size_t i : fresh)
            if (i < rows.size() && !rows[i].isSection && rows[i].origin < 0)
                rows.erase(rows.begin() + i);
    }

    void setEnabledMany(const std::vector<std::size_t>& idx, bool enabled)
    {
        if (idx.empty())
            return;
        pushUndo();
        for (std::size_t i : idx)
            if (i < rows.size() && !rows[i].isSection && !rows[i].isNew && !rows[i].deleted)
                rows[i].commented = !enabled;
    }
    void removeRow(std::size_t idx)
    {
        if (idx >= rows.size())
            return;
        pushUndo();
        if (rows[idx].origin >= 0)
            rows[idx].deleted = true;
        else
            rows.erase(rows.begin() + idx);
    }

    // --- draft ---
    winshun::Json toDraft() const
    {
        winshun::Json::Array arr;
        for (const HostsRow& r : rows) {
            winshun::Json::Object o;
            o["origin"] = r.origin;
            o["isSection"] = r.isSection;
            o["isHeader"] = r.isHeader;
            o["sectionText"] = r.sectionText;
            o["commented"] = r.commented;
            o["ip"] = r.ip;
            winshun::Json::Array hs;
            for (const std::string& h : r.hostnames)
                hs.push_back(winshun::Json(h));
            o["hostnames"] = winshun::Json(std::move(hs));
            o["comment"] = r.comment;
            o["deleted"] = r.deleted;
            o["isNew"] = r.isNew;
            o["crlf"] = r.crlf;
            arr.push_back(winshun::Json(std::move(o)));
        }
        winshun::Json::Object root;
        root["rows"] = winshun::Json(std::move(arr));
        // Rows index `base`. After a raw edit that is not the file on disk, so
        // the draft must carry it or the rows would be re-read against disk.
        if (!sameLines(base, disk))
            root["baseText"] = serializeHosts(base);
        return winshun::Json(std::move(root));
    }
    void fromDraft(const winshun::Json& d)
    {
        rows.clear();
        m_undo.clear();
        m_redo.clear();
        if (d["baseText"].isString()) {
            base = parseHosts(d["baseText"].toString());
            m_headerEnd = findHeaderEnd(base);
        } else {
            base = disk;
            m_headerEnd = findHeaderEnd(base);
        }
        for (const winshun::Json& e : d["rows"].toArray()) {
            HostsRow r;
            r.origin = static_cast<int>(e["origin"].toInt(-1));
            // The header block is owned by the base file, not the draft: a
            // draft written before folding existed carries its lines as
            // separate rows, which are dropped here and re-folded below.
            if (r.origin >= 0 && r.origin <= m_headerEnd)
                continue;
            r.isSection = e["isSection"].toBool();
            r.sectionText = e["sectionText"].toString();
            r.commented = e["commented"].toBool();
            r.ip = e["ip"].toString();
            for (const winshun::Json& h : e["hostnames"].toArray())
                r.hostnames.push_back(h.toString());
            r.comment = e["comment"].toString();
            r.deleted = e["deleted"].toBool();
            r.isNew = e["isNew"].toBool();
            r.crlf = e["crlf"].toBool();
            rows.push_back(r);
        }
        if (m_headerEnd >= 0) {
            for (std::size_t i = 0; i <= static_cast<std::size_t>(m_headerEnd) && i < base.lines.size(); ++i) {
                if (base.lines[i].text.empty())
                    continue;
                HostsRow h;
                h.origin = static_cast<int>(i);
                h.crlf = base.lines[i].crlf;
                h.isSection = true;
                h.isHeader = true;
                h.sectionText = base.lines[i].text;
                rows.insert(rows.begin(), h);
                break;
            }
        }
    }

    bool dominantCrlf() const { return m_dominantCrlf; }

    RowState rowState(std::size_t idx) const
    {
        if (idx >= rows.size())
            return RowState::Unchanged;
        const HostsRow& r = rows[idx];
        if (r.isSection)
            return RowState::Unchanged; // display-only headings
        if (r.deleted)
            return RowState::Deleted;
        if (r.isNew || r.origin < 0 || r.origin >= static_cast<int>(base.lines.size()))
            return RowState::Added;
        const HostsEntry e = parseHostLine(base.lines[r.origin].text, r.origin);
        const std::vector<std::string> baseHosts =
            e.hostnames.empty() ? std::vector<std::string> {e.hostname} : e.hostnames;
        return (e.commented != r.commented || e.ip != r.ip || e.comment != r.comment
                || baseHosts != r.hostnames)
                   ? RowState::Modified
                   : RowState::Unchanged;
    }

private:
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    bool m_dominantCrlf = false;
    int m_headerEnd = -1; // last line index of the folded header, -1 = none

    HostsEntry baseEntry(int origin) const
    {
        HostsEntry e;
        if (origin < 0 || origin >= static_cast<int>(base.lines.size()))
            return e;
        return parseHostLine(base.lines[origin].text, origin);
    }

    static bool sameLines(const HostsParsed& a, const HostsParsed& b)
    {
        if (a.bom != b.bom || a.trailingNewline != b.trailingNewline || a.lines.size() != b.lines.size())
            return false;
        for (std::size_t i = 0; i < a.lines.size(); ++i)
            if (a.lines[i].text != b.lines[i].text || a.lines[i].crlf != b.lines[i].crlf)
                return false;
        return true;
    }

    // After a raw edit the rows index a different text than the file, so the
    // change list is a plain line diff (LCS) of disk -> what would be saved.
    std::vector<HostsChange> lineDiff() const
    {
        const HostsParsed work = toParsed();
        const auto& a = disk.lines;
        const auto& b = work.lines;
        const std::size_t n = a.size(), m = b.size();
        std::vector<std::vector<int>> L(n + 1, std::vector<int>(m + 1, 0));
        for (std::size_t i = n; i-- > 0;)
            for (std::size_t j = m; j-- > 0;)
                L[i][j] = a[i].text == b[j].text ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
        std::vector<HostsChange> out;
        std::size_t i = 0, j = 0;
        auto add = [&](const char* kind, int line, const std::string& detail) {
            HostsChange c;
            c.kind = kind;
            c.line = line;
            c.detail = detail;
            out.push_back(c);
        };
        while (i < n || j < m) {
            if (i < n && j < m && a[i].text == b[j].text) {
                ++i;
                ++j;
            } else if (j < m && (i == n || L[i][j + 1] >= L[i + 1][j])) {
                add("add", static_cast<int>(j + 1), "+ " + b[j].text);
                ++j;
            } else {
                add("delete", static_cast<int>(i + 1), "- " + a[i].text);
                ++i;
            }
        }
        if (out.empty() && serializeHosts(work) != serializeHosts(disk))
            add("modify", -1, "~ line endings");
        return out;
    }

    bool rowChanged(const HostsRow& r, int origin) const
    {
        if (r.isSection)
            return r.sectionText != base.lines[origin].text;
        const HostsEntry e = baseEntry(origin);
        if (e.commented != r.commented || e.ip != r.ip || e.comment != r.comment)
            return true;
        std::vector<std::string> baseHosts = e.hostnames.empty() ? std::vector<std::string> {e.hostname} : e.hostnames;
        return baseHosts != r.hostnames;
    }

    static std::string joinHosts(const std::vector<std::string>& hs)
    {
        std::string out;
        for (std::size_t i = 0; i < hs.size(); ++i) {
            if (i)
                out += ' ';
            out += hs[i];
        }
        return out;
    }

    static std::string emitRow(const HostsRow& r)
    {
        if (r.isSection)
            return r.sectionText;
        std::string t;
        if (r.commented)
            t += "# ";
        t += r.ip;
        t += " " + joinHosts(r.hostnames);
        if (!r.comment.empty())
            t += " # " + r.comment;
        return t;
    }
};

// ---------------------------------------------------------------------------
// Three-snapshot diff for external changes (read-only, no OS dependency)
// ---------------------------------------------------------------------------
// base   = the snapshot the window opened with (EnvModel::base / HostsModel::disk)
// mine   = the current working copy (unsaved edits)
// theirs = what another process wrote while the window was open
//
// The panel only DISPLAYS these entries. It never merges anything: the user
// picks one whole-view action (reload / force overwrite / defer).
struct Diff3Entry {
    std::string key; // "user/Path"
    std::string verdict; // "mine" | "theirs" | "same" | "conflict"
    std::string base, mine, theirs;
    bool hasBase = false, hasMine = false, hasTheirs = false;
};

struct Diff3Result {
    std::vector<Diff3Entry> entries;
    int conflicts = 0;
    int theirsOnly = 0;
    int mineOnly = 0;
};

struct Diff3Val {
    std::string value;
    std::string type;
    bool present = false;
};

inline std::map<std::string, Diff3Val> diff3Normalize(const EnvSnapshot& s)
{
    std::map<std::string, Diff3Val> out;
    for (const EnvVar& v : s.vars) {
        Diff3Val d;
        d.value = v.value;
        d.type = v.type;
        d.present = true;
        // All three snapshots come from the same registry, so the original
        // spelling of a name is stable across them. Keep it for display.
        out[v.scope + "/" + v.name] = std::move(d);
    }
    return out;
}

inline std::string diff3Brief(const Diff3Val& v)
{
    if (!v.present)
        return "(absent)";
    const std::string s = v.value.empty() ? std::string("(empty)") : v.value;
    return s.size() > 52 ? s.substr(0, 52) + "…" : s;
}

// Converting rows to a snapshot lets this stay a pure function of three inputs.
inline EnvSnapshot diff3RowsToSnapshot(const std::vector<EnvVarRow>& rows)
{
    EnvSnapshot s;
    for (const EnvVarRow& r : rows) {
        if (r.deleted || r.name.empty())
            continue;
        EnvVar v;
        v.scope = r.scope;
        v.name = r.name;
        v.type = r.type;
        v.value = r.value;
        s.vars.push_back(std::move(v));
    }
    return s;
}

inline Diff3Result diff3Env(const EnvSnapshot& base, const EnvSnapshot& mine, const EnvSnapshot& theirs)
{
    const auto b = diff3Normalize(base);
    const auto m = diff3Normalize(mine);
    const auto t = diff3Normalize(theirs);
    auto eq = [](const Diff3Val& a, const Diff3Val& c) {
        return a.present == c.present && a.value == c.value && a.type == c.type;
    };
    std::vector<std::string> keys;
    for (const auto& kv : b) keys.push_back(kv.first);
    for (const auto& kv : m) keys.push_back(kv.first);
    for (const auto& kv : t) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    Diff3Result out;
    for (const std::string& k : keys) {
        auto at = [](const std::map<std::string, Diff3Val>& src, const std::string& key) {
            const auto it = src.find(key);
            return it == src.end() ? Diff3Val {} : it->second;
        };
        const Diff3Val bv = at(b, k), mv = at(m, k), tv = at(t, k);
        if (eq(mv, bv) && eq(tv, bv))
            continue; // nothing changed on either side
        Diff3Entry e;
        e.key = k;
        e.hasBase = bv.present;
        e.hasMine = mv.present;
        e.hasTheirs = tv.present;
        e.base = diff3Brief(bv);
        e.mine = diff3Brief(mv);
        e.theirs = diff3Brief(tv);
        if (eq(mv, tv)) {
            e.verdict = "same";
        } else if (eq(mv, bv)) {
            e.verdict = "theirs";
            ++out.theirsOnly;
        } else if (eq(tv, bv)) {
            e.verdict = "mine";
            ++out.mineOnly;
        } else {
            // Both sides moved away from base in different ways. A delete on
            // one side plus an edit on the other lands here too.
            e.verdict = "conflict";
            ++out.conflicts;
        }
        out.entries.push_back(std::move(e));
    }
    return out;
}

// hosts diff is SUMMARY level on purpose: an inserted line shifts every
// `origin` index, so a line-aligned unified diff would be unreliable.
struct Diff3HostsResult {
    int added = 0;
    int removed = 0;
    std::vector<std::string> samples;
};

inline Diff3HostsResult diff3Hosts(const std::string& base, const std::string& theirs)
{
    auto counts = [](const std::string& text) {
        std::map<std::string, int> c;
        std::string line;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' || text[i] == '\r') {
                if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos)
                    c[line] += 1;
                line.clear();
                continue;
            }
            line += text[i];
        }
        if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos)
            c[line] += 1;
        return c;
    };
    const auto b = counts(base), t = counts(theirs);
    Diff3HostsResult out;
    std::vector<std::string> lines;
    for (const auto& kv : b) lines.push_back(kv.first);
    for (const auto& kv : t) lines.push_back(kv.first);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    for (const std::string& l : lines) {
        const int nb = b.count(l) ? b.at(l) : 0;
        const int nt = t.count(l) ? t.at(l) : 0;
        if (nt > nb) {
            out.added += nt - nb;
            if (out.samples.size() < 8)
                out.samples.push_back("+ " + l);
        } else if (nb > nt) {
            out.removed += nb - nt;
            if (out.samples.size() < 8)
                out.samples.push_back("- " + l);
        }
    }
    return out;
}

} // namespace edit
} // namespace envh
