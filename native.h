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
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

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
#include "backup_format.h"
#include "ssh_model.h"

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

struct SshReadResult {
    bool ok = true;
    bool missing = false;
    bool utf16 = false;
    std::string content;
    std::string errCode;
    std::string errMsg;
};

inline std::wstring defaultSshDir()
{
    PWSTR knownProfile = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &knownProfile) == S_OK && knownProfile) {
        const std::wstring result = std::wstring(knownProfile) + L"\\.ssh";
        CoTaskMemFree(knownProfile);
        return result;
    }
    if (knownProfile)
        CoTaskMemFree(knownProfile);
    wchar_t profile[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, 32768);
    return n > 0 && n < 32768 ? std::wstring(profile, n) + L"\\.ssh" : std::wstring();
}

class SshStore {
public:
    SshStore(std::wstring dir, std::wstring configPath) : m_dir(std::move(dir)), m_config(std::move(configPath)) {}
    void ensureDir() { std::error_code ec; std::filesystem::create_directories(m_dir, ec); }
    SshReadResult read()
    {
        SshReadResult r;
        if (GetFileAttributesW(m_config.c_str()) == INVALID_FILE_ATTRIBUTES) { r.missing = true; return r; }
        std::ifstream in(m_config, std::ios::binary);
        if (!in) { r.ok = false; r.errCode = "E_SSH_READ"; r.errMsg = "cannot read the SSH config"; return r; }
        r.content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool le = r.content.size() >= 2 && static_cast<unsigned char>(r.content[0]) == 0xFF && static_cast<unsigned char>(r.content[1]) == 0xFE;
        const bool be = r.content.size() >= 2 && static_cast<unsigned char>(r.content[0]) == 0xFE && static_cast<unsigned char>(r.content[1]) == 0xFF;
        if (le || be) { r.ok = false; r.utf16 = true; r.errCode = "E_SSH_UTF16"; r.errMsg = "the SSH config is UTF-16; convert it to UTF-8 before editing"; }
        return r;
    }
    SshReadResult readKnownHosts() const
    {
        SshReadResult r; const std::wstring path = m_dir + L"\\known_hosts";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { r.missing = true; return r; }
        std::ifstream in(path, std::ios::binary); if (!in) { r.ok = false; r.errCode = "E_SSH_READ"; r.errMsg = "cannot read known_hosts"; return r; }
        r.content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); return r;
    }
    bool write(const std::string& content, std::string& error) { return writeFile(m_config, content, error); }
    bool writeKnownHosts(const std::string& content, std::string& error) { return writeFile(m_dir + L"\\known_hosts", content, error); }
    std::vector<std::string> fileNames() const
    {
        std::vector<std::string> out; std::error_code ec; if (!std::filesystem::exists(m_dir, ec)) return out;
        for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) if (entry.is_regular_file(ec)) out.push_back(entry.path().filename().u8string());
        std::sort(out.begin(), out.end()); return out;
    }
    std::vector<ssh::KeyFile> keys() const
    {
        std::vector<ssh::KeyFile> out; std::error_code ec; if (!std::filesystem::exists(m_dir, ec)) return out;
        std::vector<std::string> pubs, privs;
        for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue; const auto name = entry.path().filename().u8string();
            if (name == "config" || name == "known_hosts" || name == "known_hosts.old" || name == "known_hosts2") continue;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pub") == 0) pubs.push_back(name);
            else { std::ifstream in(entry.path(), std::ios::binary); std::string head(64, '\0'); in.read(head.data(), 63); head.resize(static_cast<size_t>(in.gcount())); if (head.find("PRIVATE KEY-----") != std::string::npos) privs.push_back(name); }
        }
        std::sort(pubs.begin(), pubs.end()); std::sort(privs.begin(), privs.end()); std::vector<bool> paired(privs.size(), false);
        for (const auto& pub : pubs) { const auto stem = pub.substr(0, pub.size() - 4); const auto it = std::find(privs.begin(), privs.end(), stem); const bool has = it != privs.end(); if (has) paired[static_cast<size_t>(it - privs.begin())] = true; std::ifstream in(m_dir / std::filesystem::path(pub), std::ios::binary); const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); out.push_back(ssh::makeKeyFile(has ? stem : pub, has, true, text)); }
        for (size_t i = 0; i < privs.size(); ++i) if (!paired[i]) out.push_back(ssh::makeKeyFile(privs[i], true, false, "")); return out;
    }
    bool deleteKeys(const std::vector<std::string>& names, std::string& error) { for (const auto& name : names) { if (!ssh::validKeyFileName(name)) { error = "E_KEYNAME"; return false; } const auto path = m_dir / std::filesystem::path(name); if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) { error = "E_SSH_DELETE"; return false; } } return true; }
private:
    bool writeFile(const std::wstring& path, const std::string& content, std::string& error) { std::error_code ec; std::filesystem::create_directories(m_dir, ec); const HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (fh == INVALID_HANDLE_VALUE) { error = GetLastError() == ERROR_ACCESS_DENIED ? "E_DENIED" : "E_SSH_WRITE"; return false; } SetFilePointer(fh, 0, nullptr, FILE_BEGIN); SetEndOfFile(fh); DWORD wrote = 0; const BOOL ok = WriteFile(fh, content.data(), static_cast<DWORD>(content.size()), &wrote, nullptr); CloseHandle(fh); if (!ok || wrote != content.size()) { error = "E_SSH_WRITE"; return false; } return true; }
    std::wstring m_dir, m_config;
};

struct ProcResult { bool launched = false; bool ok = false; int exitCode = -1; std::string out; std::string err; };
struct LaunchResult { bool launched = false; std::string err; };
inline std::wstring findProgram(const wchar_t* name) { wchar_t found[32768]{}; const DWORD n = SearchPathW(nullptr, name, nullptr, 32768, found, nullptr); return n ? std::wstring(found, n) : std::wstring(); }
inline std::wstring findSshKeygen() { const auto path = findProgram(L"ssh-keygen.exe"); if (!path.empty()) return path; const std::wstring bundled = L"C:\\Windows\\System32\\OpenSSH\\ssh-keygen.exe"; return GetFileAttributesW(bundled.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : bundled; }
inline std::wstring findSshAdd() { const auto path = findProgram(L"ssh-add.exe"); if (!path.empty()) return path; const std::wstring bundled = L"C:\\Windows\\System32\\OpenSSH\\ssh-add.exe"; return GetFileAttributesW(bundled.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : bundled; }
inline std::wstring quoteLaunchArg(const std::wstring& arg) { if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg; std::wstring out=L"\""; int slashes=0; for(const wchar_t c:arg){if(c==L'\\')++slashes;else if(c==L'\"'){out.append(static_cast<size_t>(slashes*2+1),L'\\');out.push_back(L'\"');slashes=0;}else{out.append(static_cast<size_t>(slashes),L'\\');slashes=0;out.push_back(c);}}out.append(static_cast<size_t>(slashes*2),L'\\');out.push_back(L'\"');return out; }
inline LaunchResult launchProcess(const std::wstring& program, const std::vector<std::wstring>& args) { LaunchResult result; std::wstring command=quoteLaunchArg(program);for(const auto& arg:args)command+=L" "+quoteLaunchArg(arg);STARTUPINFOW si{};si.cb=sizeof(si);si.lpDesktop=const_cast<wchar_t*>(L"winsta0\\default");PROCESS_INFORMATION pi{};if(!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT|CREATE_NEW_CONSOLE,nullptr,nullptr,&si,&pi)){result.err="cannot start process";return result;}CloseHandle(pi.hThread);CloseHandle(pi.hProcess);result.launched=true;return result; }
inline std::vector<wchar_t> processEnvironmentWith(const wchar_t* name, const std::wstring& value)
{
    std::vector<std::wstring> entries;
    LPWCH environment = GetEnvironmentStringsW();
    if (environment) {
        for (const wchar_t* entry = environment; *entry; entry += std::wcslen(entry) + 1) {
            const wchar_t* equals = std::wcschr(entry, L'=');
            if (!equals || _wcsnicmp(entry, name, static_cast<size_t>(equals - entry)) != 0
                || static_cast<size_t>(equals - entry) != std::wcslen(name))
                entries.emplace_back(entry);
        }
        FreeEnvironmentStringsW(environment);
    }
    entries.emplace_back(std::wstring(name) + L"=" + value);
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

inline ProcResult runInteractiveProcess(const std::wstring& program, const std::vector<std::wstring>& args,
                                        const std::wstring& sshAuthSock = {})
{
    ProcResult r;
    std::wstring command = quoteLaunchArg(program);
    for (const auto& arg : args) command += L" " + quoteLaunchArg(arg);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> environment;
    void* environmentBlock = nullptr;
    if (!sshAuthSock.empty()) {
        environment = processEnvironmentWith(L"SSH_AUTH_SOCK", sshAuthSock);
        environmentBlock = environment.data();
    }
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, environmentBlock, nullptr, &si, &pi)) {
        r.err = "cannot start process"; return r;
    }
    r.launched = true;
    WaitForSingleObject(pi.hProcess, 300000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    r.exitCode = static_cast<int>(code); r.ok = code == 0; return r;
}
// Shell "open file" picker. The QML front-end runs on WinShun's shared host Qt,
// which does not ship the Qt Quick Dialogs module, so the picker comes from the
// shell instead of a QML dialog.
inline std::wstring pickOpenFile(const std::wstring& title, const std::wstring& startDir, HWND owner)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring picked;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        if (!title.empty()) dialog->SetTitle(title.c_str());
        if (!startDir.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    picked = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return picked;
}
inline bool openUrl(const std::wstring& url, std::string& error) { const HINSTANCE r=ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(reinterpret_cast<INT_PTR>(r)<=32){error="cannot open URL";return false;}return true; }
inline ProcResult runProcess(const std::wstring& program, const std::vector<std::wstring>& args) { ProcResult r; std::wstring command=quoteLaunchArg(program);for(const auto& arg:args)command+=L" "+quoteLaunchArg(arg);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};if(!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){r.err="cannot start process";return r;}r.launched=true;WaitForSingleObject(pi.hProcess,60000);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);r.exitCode=static_cast<int>(code);r.ok=code==0;return r; }
inline ProcResult runProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                                     const std::wstring& sshAuthSock = {})
{
    ProcResult r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) { r.err = "cannot create process pipe"; return r; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = quoteLaunchArg(program);
    for (const auto& arg : args) command += L" " + quoteLaunchArg(arg);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE; si.hStdOutput = writePipe; si.hStdError = writePipe; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> environment;
    void* environmentBlock = nullptr;
    if (!sshAuthSock.empty()) {
        environment = processEnvironmentWith(L"SSH_AUTH_SOCK", sshAuthSock);
        environmentBlock = environment.data();
    }
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environmentBlock, nullptr, &si, &pi)) {
        CloseHandle(readPipe); CloseHandle(writePipe); r.err = "cannot start process"; return r;
    }
    CloseHandle(writePipe); r.launched = true;
    char buffer[4096]; DWORD got = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) r.out.append(buffer, got);
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    r.exitCode = static_cast<int>(code); r.ok = code == 0; return r;
}

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
        root["schemaVersion"] = 1;
        winshun::Json::Array scopes{"user"};
        if (includeSystem) scopes.push_back("system");
        root["scopes"] = winshun::Json(std::move(scopes));
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

    std::string saveSsh(const std::string& content)
    {
        const std::string id = stamp();
        const std::string path = m_dir + "/backups/ssh/" + id + ".config";
        ensureDir(m_dir + "/backups/ssh");
        writeFile(path, content);
        prune(m_dir + "/backups/ssh");
        return id;
    }

    std::string saveKnownHosts(const std::string& content)
    {
        const std::string id = stamp();
        const std::string path = m_dir + "/backups/ssh/" + id + ".known_hosts";
        ensureDir(m_dir + "/backups/ssh");
        writeFile(path, content);
        prune(m_dir + "/backups/ssh");
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

    // ---- v0.6.0: read-only inspection and per-backup operations -------------
    // Ids come from our own millisecond stamp(). Reject anything else so a
    // crafted id can never escape the backup directory.
    static bool validId(const std::string& id)
    {
        if (id.empty() || id.size() > 64 || id.find("..") != std::string::npos)
            return false;
        for (char c : id)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9') || c == '.' || c == '-'))
                return false;
        return true;
    }

    std::string backupPath(const std::string& kind, const std::string& id) const
    {
        if (!validId(id))
            return {};
        if (kind == "env")
            return m_dir + "/backups/env/" + id + ".json";
        if (kind == "hosts")
            return m_dir + "/backups/hosts/" + id + ".hosts";
        return {};
    }

    EnvSnapshot readEnv(const std::string& id)
    {
        EnvSnapshot snapshot;
        std::set<std::string> scopes;
        std::string error;
        readEnvChecked(id, snapshot, scopes, error);
        return snapshot;
    }

    bool readEnvChecked(const std::string& id, EnvSnapshot& snapshot, std::set<std::string>& scopes, std::string& error)
    {
        const std::string path = backupPath("env", id);
        if (path.empty()) { error="E_BACKUP_ID";return false; }
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        if (!in) { error="E_BACKUP_READ";return false; }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto root=winshun::Json::parse(text);
        if (!root) { error="E_BACKUP_FORMAT";return false; }
        return decodeGdiBackup(*root, snapshot, scopes, error);
    }

    std::string readHosts(const std::string& id)
    {
        const std::string path = backupPath("hosts", id);
        if (path.empty())
            return {};
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // Delete exactly one backup file. Returns false for an unknown/invalid id.
    bool remove(const std::string& kind, const std::string& id)
    {
        const std::string path = backupPath(kind, id);
        if (path.empty())
            return false;
        std::error_code ec;
        return std::filesystem::remove(std::filesystem::u8path(path), ec);
    }

    // Copy one backup into WS_DATA_DIR/exports so the user can open it outside
    // the editor. Returns the written path, or "" on failure.
    std::string exportTo(const std::string& kind, const std::string& id)
    {
        const std::string path = backupPath(kind, id);
        if (path.empty())
            return {};
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        const std::string dir = m_dir + "/exports";
        ensureDir(dir);
        const std::string out = dir + "/" + id + (kind == "env" ? ".json" : ".hosts");
        std::ofstream dst(std::filesystem::u8path(out), std::ios::binary);
        if (!dst)
            return {};
        dst << in.rdbuf();
        return out;
    }

    // Restore user-scope vars to a backup (system scope needs elevated, skipped).
    bool restoreEnv(const std::string& id, EnvStore& env)
    {
        EnvSnapshot backup;std::set<std::string> scopes;std::string error;
        if (!readEnvChecked(id, backup, scopes, error) || !scopes.count("user"))
            return false;
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
        if (!validId(id))
            return false;
        const std::string content = readHosts(id);
        std::string err;
        return !content.empty() && hosts.write(content, err);
    }

private:
    static EnvSnapshot parseEnvBackup(const std::string& text)
    {
        EnvSnapshot snap;
        const auto parsed = winshun::Json::parse(text);
        if (!parsed || !parsed->isObject())
            return snap;
        for (const winshun::Json& v : (*parsed)["vars"].toArray()) {
            EnvVar e;
            e.scope = v["scope"].toString();
            e.name = v["name"].toString();
            e.type = v["type"].toString();
            e.value = v["value"].toString();
            snap.vars.push_back(std::move(e));
        }
        return snap;
    }
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
        const std::string suffix = dir.find("ssh") != std::string::npos ? ".config" : (dir.find("hosts") != std::string::npos ? ".hosts" : ".json");
        std::vector<Entry> all = listDir(dir, suffix);
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
