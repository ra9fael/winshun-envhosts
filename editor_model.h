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

// ---------------------------------------------------------------------------
// PATH helpers (env list-mode editing)
// ---------------------------------------------------------------------------

inline std::vector<std::string> splitDelimited(const std::string& value, const std::string& delimiter, bool preserveQuoted)
{
    if (delimiter.empty())
        return {value};
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

    // Validation: any invalid name/value blocks save. System rows are read-only
    // and ignored (cannot be edited in the window anyway).
    bool hasInvalid() const
    {
        for (const EnvVarRow& r : rows) {
            if (r.deleted || r.scope == "system")
                continue;
            if (r.name.empty() || !isValidEnvName(r.name))
                return true;
            if (!isValidEnvValue(r.value))
                return true;
        }
        return false;
    }

    // Produce the user-scope delta a save would apply.
    struct Delta {
        std::vector<EnvVarRow> toWrite; // user scope only
        std::vector<std::string> toDelete; // user-scope names
    };
    Delta delta() const
    {
        Delta d;
        for (const EnvVarRow& r : rows) {
            if (r.scope != "user")
                continue;
            if (r.deleted) {
                if (!r.isNew)
                    d.toDelete.push_back(r.name);
                continue;
            }
            bool inBase = false;
            for (const EnvVarRow& b : base)
                if (toLower(r.name) == toLower(b.name) && b.scope == "user")
                    inBase = true;
            if (!inBase || r.isNew) {
                d.toWrite.push_back(r);
            } else {
                // only rewrite if actually changed
                const EnvVarRow* bptr = nullptr;
                for (const EnvVarRow& b : base)
                    if (toLower(r.name) == toLower(b.name) && b.scope == "user") {
                        bptr = &b;
                        break;
                    }
                if (!bptr || bptr->value != r.value || bptr->type != r.type)
                    d.toWrite.push_back(r);
            }
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
    HostsParsed base; // committed; untouched lines re-emitted byte-exact
    std::vector<HostsRow> rows; // working copy, in file order
    std::string fingerprint; // FNV over content + size

    static std::string contentFingerprint(const std::string& content)
    {
        return std::to_string(content.size()) + ":" + fnv1aHex(content);
    }

    static HostsModel fromParsed(const HostsParsed& h, bool dominantCrlf)
    {
        HostsModel m;
        m.base = h;
        m.fingerprint = contentFingerprint(serializeHosts(h));
        for (const HostsLine& line : h.lines) {
            if (line.text.empty())
                continue; // blank lines kept by line index, not as rows
            const HostsEntry e = parseHostLine(line.text, static_cast<int>(&line - &h.lines[0]));
            HostsRow r;
            r.origin = static_cast<int>(&line - &h.lines[0]);
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
            m.rows.push_back(r);
        }
        m.m_dominantCrlf = dominantCrlf;
        return m;
    }

    bool isDirty() const
    {
        for (const HostsRow& r : rows) {
            if (r.isNew || r.deleted)
                return true;
            const HostsEntry e = baseEntry(r.origin);
            if (e.raw.empty() && !r.isSection)
                return true; // base had nothing here
            if (r.isSection) {
                if (r.sectionText != base.lines[r.origin].text)
                    return true;
                continue;
            }
            if (e.commented != r.commented || e.ip != r.ip || e.comment != r.comment)
                return true;
            std::vector<std::string> baseHosts = e.hostnames.empty() ? std::vector<std::string> {e.hostname} : e.hostnames;
            if (baseHosts != r.hostnames)
                return true;
        }
        return false;
    }

    std::vector<HostsChange> changeList() const
    {
        std::vector<HostsChange> out;
        for (const HostsRow& r : rows) {
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
        for (std::size_t i = 0; i < base.lines.size(); ++i) {
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
            if (!rowChanged(r, i)) {
                out.lines.push_back(base.lines[i]); // byte-exact
            } else {
                HostsLine nl;
                nl.text = emitRow(r);
                nl.crlf = r.origin >= 0 ? base.lines[i].crlf : m_dominantCrlf;
                out.lines.push_back(nl);
            }
        }
        // Append new rows at the end.
        for (const HostsRow& r : rows) {
            if (r.origin != -1 || r.deleted)
                continue;
            HostsLine nl;
            nl.text = emitRow(r);
            nl.crlf = m_dominantCrlf;
            out.lines.push_back(nl);
        }
        // Keep a blank separator before the first appended new row if needed.
        return out;
    }

    std::string serializeWork() const { return serializeHosts(toParsed()); }

    // --- mutations ---
    void pushUndo()
    {
        m_undo.push_back(rows);
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
        m_redo.push_back(rows);
        rows = m_undo.back();
        m_undo.pop_back();
    }
    void redo()
    {
        if (m_redo.empty())
            return;
        m_undo.push_back(rows);
        rows = m_redo.back();
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
        return winshun::Json(std::move(root));
    }
    void fromDraft(const winshun::Json& d)
    {
        rows.clear();
        m_undo.clear();
        m_redo.clear();
        for (const winshun::Json& e : d["rows"].toArray()) {
            HostsRow r;
            r.origin = static_cast<int>(e["origin"].toInt(-1));
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
    }

    bool dominantCrlf() const { return m_dominantCrlf; }

private:
    std::vector<std::vector<HostsRow>> m_undo;
    std::vector<std::vector<HostsRow>> m_redo;
    bool m_dominantCrlf = false;

    HostsEntry baseEntry(int origin) const
    {
        HostsEntry e;
        if (origin < 0 || origin >= static_cast<int>(base.lines.size()))
            return e;
        return parseHostLine(base.lines[origin].text, origin);
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

} // namespace edit
} // namespace envh
