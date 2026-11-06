// native.h - Win32 I/O for the env / hosts WinShun plugin.
//
// This file is compiled only on Windows. It owns every OS call the plugin makes:
// registry reads/writes for environment variables, byte-exact hosts file reads
// and writes, the WM_SETTINGCHANGE broadcast, and the backup store under
// WS_DATA_DIR. logic.h stays OS-free so it can be unit-tested without Windows;
// main.cpp wires logic.h to native.h.
//
// Safety notes honoured here:
//  - Registry writes use RegSetValueEx directly (never setx, which silently
//    truncates values at 1024 chars and guesses the type).
//  - The existing value type (REG_SZ vs REG_EXPAND_SZ) is preserved.
//  - hosts writes are in-place (open + truncate + write) so the file ACL is kept;
//    a probe decides writability before any byte is written.
//  - x64 is mandatory: a 32-bit build is redirected by the file-system
//    redirector to SysWOW64 and would edit the wrong hosts file.
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "winshun.h"
#include "logic.h"

namespace envh {
namespace native {

// One process-wide lock around every registry/hosts read-modify-write so the
// launcher's inline actions (protocol thread) and the editor window (GUI thread)
// never race on the same target.
inline std::mutex g_writeLock;



inline std::wstring toWide(const std::string& s) { return winshun::wide(s); }
inline std::string fromWide(const std::wstring& w) { return winshun::utf8(w); }

// ---- environment registry -----------------------------------------------------

struct EnvReadResult {
    bool ok = true;
    EnvSnapshot snap;
    std::string errCode; // only when ok == false
    std::string errMsg;
};

// Whether this process runs with an elevated (administrator) token. With the
// `elevated` permission Win顺 starts the plugin elevated only when Win顺 itself
// runs as administrator and the user confirmed the plugin card; it never shows
// UAC for a plugin. Used for the status text, not as the write gate (the gate
// is the real canWriteSystem / isWritable probe).
inline bool processElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION e {};
    DWORD got = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got);
    CloseHandle(token);
    return ok && e.TokenIsElevated != 0;
}

// The HKLM subkey holding the system-scope variables. Named so the test-only
// override in main.cpp can fall back to exactly this path.
inline std::wstring systemEnvSubkey()
{
    return L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
}

// Root of a system-scope key path: HKLM unless the path spells out its own root
// ("HKCU\..."), which only the test-only override does so the 系统 tab can show
// fake rows instead of this machine's real variables. `skip` is the prefix
// length to drop before opening.
inline HKEY systemEnvRoot(const std::wstring& sub, size_t& skip)
{
    static const std::pair<const wchar_t*, HKEY> kRoots[]
        = {{L"HKCU\\", HKEY_CURRENT_USER}, {L"HKLM\\", HKEY_LOCAL_MACHINE}};
    for (const auto& r : kRoots) {
        const size_t n = std::wcslen(r.first);
        if (sub.size() > n && _wcsnicmp(sub.data(), r.first, n) == 0) {
            skip = n;
            return r.second;
        }
    }
    skip = 0;
    return HKEY_LOCAL_MACHINE;
}

class EnvStore {
public:
    explicit EnvStore(std::wstring userSubkey = L"Environment",
                      std::wstring sysSubkey = systemEnvSubkey())
        : m_user(std::move(userSubkey)), m_sys(std::move(sysSubkey))
    {
    }

    // Reads user-scope (HKCU) and, when includeSystem is set, system-scope
    // (HKLM). A system read that is denied is reported via errCode but does not
    // fail the whole read: the user list is still returned.
    EnvReadResult read(bool includeSystem)
    {
        EnvReadResult r;
        // User scope.
        {
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, m_user.c_str(), 0, KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &h) == ERROR_SUCCESS) {
                readValues(h, "user", r.snap);
                RegCloseKey(h);
            }
        }
        // System scope (read-only in v0.1; needs elevated to write).
        if (includeSystem) {
            HKEY h = nullptr;
            size_t skip = 0;
            const HKEY root = systemEnvRoot(m_sys, skip);
            const LONG rc = RegOpenKeyExW(root, m_sys.c_str() + skip, 0,
                                          KEY_READ | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &h);
            if (rc == ERROR_SUCCESS) {
                readValues(h, "system", r.snap);
                RegCloseKey(h);
            } else {
                r.errCode = "E_SYSTEM_READ";
                r.errMsg = "system variables not readable (rc=" + std::to_string(rc) + ")";
            }
        }
        return r;
    }

    bool canWriteUser(std::string& errCode)
    {
        HKEY h = nullptr;
        const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, m_user.c_str(), 0, KEY_SET_VALUE, &h);
        if (rc == ERROR_SUCCESS) {
            RegCloseKey(h);
            return true;
        }
        errCode = "E_DENIED";
        return false;
    }

    bool canWriteSystem(std::string& errCode)
    {
        HKEY h = nullptr;
        size_t skip = 0;
        const HKEY root = systemEnvRoot(m_sys, skip);
        const LONG rc = RegOpenKeyExW(root, m_sys.c_str() + skip, 0, KEY_SET_VALUE, &h);
        if (rc == ERROR_SUCCESS) {
            RegCloseKey(h);
            return true;
        }
        errCode = "E_DENIED";
        return false;
    }

    // Writes go to the user key, or to the system key for scope "system".
    // System writes only succeed when the process is elevated (the plugin
    // declares `elevated` and Win顺 runs as administrator); the caller probes
    // with canWriteSystem() first and the open below fails cleanly otherwise.
    bool openForWrite(const std::string& scope, HKEY& h)
    {
        if (scope == "system") {
            size_t skip = 0;
            const HKEY root = systemEnvRoot(m_sys, skip);
            return RegOpenKeyExW(root, m_sys.c_str() + skip, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS;
        }
        return RegOpenKeyExW(HKEY_CURRENT_USER, m_user.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS;
    }

    bool writeVar(const std::string& name, const std::string& value, const std::string& type,
                  const std::string& scope = "user")
    {
        HKEY h = nullptr;
        if (!openForWrite(scope, h))
            return false;
        const std::wstring wname = toWide(name);
        const std::wstring wval = toWide(value);
        DWORD dwType = REG_SZ;
        const BYTE* data = reinterpret_cast<const BYTE*>(wval.c_str());
        DWORD cb = static_cast<DWORD>((wval.size() + 1) * sizeof(wchar_t));
        if (type == "REG_EXPAND_SZ")
            dwType = REG_EXPAND_SZ;
        else if (type == "REG_MULTI_SZ") {
            dwType = REG_MULTI_SZ;
            std::wstring multi;
            // Split value on ';' and re-terminate each component with NUL.
            std::wstring cur;
            for (wchar_t c : wval) {
                if (c == L';') {
                    multi += cur + L'\0';
                    cur.clear();
                } else
                    cur += c;
            }
            multi += cur + L'\0' + L'\0';
            data = reinterpret_cast<const BYTE*>(multi.c_str());
            cb = static_cast<DWORD>(multi.size() * sizeof(wchar_t));
        }
        const LONG rc = RegSetValueExW(h, wname.c_str(), 0, dwType, data, cb);
        RegCloseKey(h);
        return rc == ERROR_SUCCESS;
    }

    bool deleteVar(const std::string& name, const std::string& scope = "user")
    {
        HKEY h = nullptr;
        if (!openForWrite(scope, h))
            return false;
        const LONG rc = RegDeleteValueW(h, toWide(name).c_str());
        RegCloseKey(h);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }

    void broadcastSettingsChange()
    {
        // Notify Explorer and other listeners that the environment changed.
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                            reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 3000, nullptr);
    }

private:
    static void readValues(HKEY h, const std::string& scope, EnvSnapshot& snap)
    {
        DWORD count = 0;
        RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr, nullptr);
        for (DWORD i = 0; i < count; ++i) {
            wchar_t nameBuf[16384] = {0};
            DWORD nameLen = 16383;
            DWORD cb = 0;
            DWORD type = 0;
            // First call: learn the value size. This shrinks nameLen to the
            // name length (without the NUL), so it must be reset before the
            // second call or RegEnumValueW fails with ERROR_MORE_DATA.
            LONG rc = RegEnumValueW(h, i, nameBuf, &nameLen, nullptr, &type, nullptr, &cb);
            if (rc != ERROR_SUCCESS)
                continue;
            nameLen = 16383;
            std::wstring wdata(static_cast<std::size_t>((cb / sizeof(wchar_t)) + 2), L'\0');
            rc = RegEnumValueW(h, i, nameBuf, &nameLen, nullptr, &type,
                               reinterpret_cast<BYTE*>(&wdata[0]), &cb);
            if (rc != ERROR_SUCCESS)
                continue;
            EnvVar v;
            v.scope = scope;
            v.name = fromWide(nameBuf);
            if (type == REG_EXPAND_SZ)
                v.type = "REG_EXPAND_SZ";
            else if (type == REG_MULTI_SZ) {
                v.type = "REG_MULTI_SZ";
                // Join components with ';'.
                const wchar_t* p = wdata.c_str();
                std::wstring joined;
                while (*p) {
                    if (!joined.empty())
                        joined += L';';
                    joined += p;
                    p += std::wcslen(p) + 1;
                }
                v.value = fromWide(joined);
            } else
                v.type = "REG_SZ";
            if (type != REG_MULTI_SZ)
                v.value = fromWide(wdata.c_str());
            snap.vars.push_back(std::move(v));
        }
    }

    std::wstring m_user;
    std::wstring m_sys;
};

// ---- hosts file ---------------------------------------------------------------

struct HostsReadResult {
    bool ok = true;
    std::string content;
    bool utf16 = false;
    std::string errCode;
    std::string errMsg;
};

class HostsStore {
public:
    explicit HostsStore(std::wstring path) : m_path(std::move(path)) {}

    HostsReadResult read()
    {
        HostsReadResult r;
        const HANDLE fh = CreateFileW(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) {
            const DWORD rc = GetLastError();
            if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) {
                r.ok = false;
                r.errCode = "E_HOSTS_MISSING";
                r.errMsg = "hosts file not found at the configured path";
            } else if (rc == ERROR_SHARING_VIOLATION) {
                r.ok = false;
                r.errCode = "E_HOSTS_BUSY";
                r.errMsg = "hosts file is locked by another process";
            } else {
                r.ok = false;
                r.errCode = "E_HOSTS_READ";
                r.errMsg = "cannot read hosts (rc=" + std::to_string(rc) + ")";
            }
            return r;
        }
        LARGE_INTEGER size;
        std::string buf;
        if (GetFileSizeEx(fh, &size) && size.QuadPart > 0) {
            buf.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD got = 0;
            ReadFile(fh, &buf[0], static_cast<DWORD>(buf.size()), &got, nullptr);
            buf.resize(got);
        }
        CloseHandle(fh);
        // Refuse to touch UTF-16 hosts (Notepad can save that); read shows it but
        // writes are blocked to avoid corrupting the encoding.
        if (buf.size() >= 2 && static_cast<unsigned char>(buf[0]) == 0xFF
            && static_cast<unsigned char>(buf[1]) == 0xFE) {
            r.ok = false;
            r.utf16 = true;
            r.errCode = "E_HOSTS_UTF16";
            r.errMsg = "hosts is UTF-16; convert to UTF-8 before editing";
            return r;
        }
        r.content = std::move(buf);
        return r;
    }

    // Probe writability without writing a byte (used for the lock marker).
    bool isWritable(std::string& errCode)
    {
        const HANDLE fh = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) {
            const DWORD rc = GetLastError();
            if (rc == ERROR_ACCESS_DENIED)
                errCode = "E_DENIED";
            else
                errCode = "E_HOSTS_WRITE";
            return false;
        }
        CloseHandle(fh);
        return true;
    }

    bool write(const std::string& content, std::string& errCode)
    {
        const HANDLE fh = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh == INVALID_HANDLE_VALUE) {
            const DWORD rc = GetLastError();
            errCode = (rc == ERROR_ACCESS_DENIED) ? "E_DENIED" : "E_HOSTS_WRITE";
            return false;
        }
        // In-place rewrite preserves the file ACL.
        SetFilePointer(fh, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(fh);
        DWORD wrote = 0;
        const BOOL ok = WriteFile(fh, content.data(), static_cast<DWORD>(content.size()), &wrote, nullptr);
        CloseHandle(fh);
        if (!ok || wrote != content.size()) {
            errCode = "E_HOSTS_WRITE";
            return false;
        }
        return true;
    }

private:
    std::wstring m_path;
};

// ---- backups ------------------------------------------------------------------

class BackupStore {
public:
    BackupStore(std::string dataDir, int keep) : m_dir(std::move(dataDir)), m_keep(keep > 0 ? keep : 10)
    {
        ensureDir(m_dir + "/backups/env");
        ensureDir(m_dir + "/backups/hosts");
    }

    // System-scope values are included only when the caller is about to write
    // that scope (elevated plugin), so a bad HKLM write can be undone too.
    std::string saveEnv(const EnvSnapshot& snap, bool includeSystem = false)
    {
        const std::string id = stamp();
        winshun::Json::Object root;
        winshun::Json::Array vars;
        for (const EnvVar& v : snap.vars) {
            if (v.scope != "user" && !(includeSystem && v.scope == "system"))
                continue;
            winshun::Json::Object o;
            o["name"] = v.name;
            o["type"] = v.type;
            o["value"] = v.value;
            o["scope"] = v.scope;
            vars.push_back(winshun::Json(std::move(o)));
        }
        root["vars"] = winshun::Json(std::move(vars));
        const std::string path = m_dir + "/backups/env/" + id + ".json";
        writeFile(path, root.empty() ? std::string("{\"vars\":[]}") : winshun::Json(std::move(root)).dump());
        prune(m_dir + "/backups/env");
        return id;
    }

    std::string saveHosts(const std::string& content)
    {
        const std::string id = stamp();
        const std::string path = m_dir + "/backups/hosts/" + id + ".hosts";
        writeFile(path, content);
        prune(m_dir + "/backups/hosts");
        return id;
    }

    struct Entry {
        std::string id;
        std::string path;
    };

    std::vector<Entry> listEnv()
    {
        return listDir(m_dir + "/backups/env", ".json");
    }
    std::vector<Entry> listHosts()
    {
        return listDir(m_dir + "/backups/hosts", ".hosts");
    }

    // Restore user-scope vars to a backup (system scope needs elevated, skipped).
    bool restoreEnv(const std::string& id, EnvStore& env)
    {
        const std::string path = m_dir + "/backups/env/" + id + ".json";
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto parsed = winshun::Json::parse(text);
        if (!parsed || !parsed->isObject())
            return false;
        EnvSnapshot backup;
        for (const winshun::Json& v : (*parsed)["vars"].toArray()) {
            EnvVar e;
            e.scope = v["scope"].toString();
            e.name = v["name"].toString();
            e.type = v["type"].toString();
            e.value = v["value"].toString();
            backup.vars.push_back(std::move(e));
        }
        EnvSnapshot current = env.read(false).snap;
        // Apply backup vars.
        for (const EnvVar& b : backup.vars) {
            if (b.scope != "user")
                continue;
            env.writeVar(b.name, b.value, b.type);
        }
        // Delete current user vars not present in the backup.
        for (const EnvVar& c : current.vars) {
            if (c.scope != "user")
                continue;
            if (!findEnvVar(backup, c.name, "user"))
                env.deleteVar(c.name);
        }
        env.broadcastSettingsChange();
        return true;
    }

    bool restoreHosts(const std::string& id, HostsStore& hosts)
    {
        const std::string path = m_dir + "/backups/hosts/" + id + ".hosts";
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        return hosts.write(content, err);
    }

private:
    static void ensureDir(const std::string& p)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(p), ec);
    }
    static void writeFile(const std::string& path, const std::string& data)
    {
        std::ofstream out(std::filesystem::u8path(path), std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::vector<Entry> listDir(const std::string& dir, const std::string& suffix)
    {
        std::vector<Entry> out;
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(dir), ec))
            return out;
        for (const auto& ent : std::filesystem::directory_iterator(std::filesystem::u8path(dir))) {
            const std::string name = ent.path().filename().string();
            if (ent.path().string().size() >= suffix.size()
                && ent.path().string().compare(ent.path().string().size() - suffix.size(), suffix.size(), suffix) == 0) {
                Entry e;
                e.id = name.substr(0, name.size() - suffix.size());
                e.path = ent.path().string();
                out.push_back(std::move(e));
            }
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.id > b.id; });
        return out;
    }
    void prune(const std::string& dir)
    {
        std::vector<Entry> all = listDir(dir, dir.find("hosts") != std::string::npos ? ".hosts" : ".json");
        while (all.size() > static_cast<std::size_t>(m_keep)) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(all.back().path), ec);
            all.pop_back();
        }
    }
    std::string stamp()
    {
        // Millisecond resolution: two saves inside one second would otherwise
        // produce the same file name and silently overwrite the older snapshot,
        // losing an undo point.
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tmbuf {};
        gmtime_s(&tmbuf, &t);
        char buf[40];
        std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%S", &tmbuf);
        char msBuf[8];
        std::snprintf(msBuf, sizeof msBuf, ".%03dZ", ms);
        return std::string(buf) + msBuf;
    }

    std::string m_dir;
    int m_keep;
};

} // namespace native
} // namespace envh

#endif // _WIN32
