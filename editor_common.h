// editor_common.h - arguments shared by the two editor front-ends.
//
// The protocol thread builds an OpenArgs and hands it to whichever front-end the
// build compiles in: the Win32 GDI window (editor_win.h) or the Qt Quick
// prototype (qt_editor.h). Keeping the struct in one place stops the two from
// drifting apart while the QML migration is in progress.
//
// Code and comments are English.
#pragma once

#ifdef _WIN32

#include <string>

#include "winshun.h"
#include "native.h"
#include "i18n.h"

namespace editorwin {

struct OpenArgs {
    std::string kind; // "env" | "hosts"
    std::wstring userSubkey = L"Environment"; // HKCU subkey (test override allowed)
    std::wstring sysSubkey = envh::native::systemEnvSubkey(); // HKLM subkey (test override allowed)
    std::wstring hostsPath; // full path to hosts
    std::string dataDir; // WS_DATA_DIR (backups/ and drafts/<kind>.json live here)
    int backupCount = 10;
    bool showSystem = true;
    std::string filter;
    std::string select; // "user/NAME" (env) or 1-based line (hosts)
    winshun::Json draft; // empty => ignore
    // Resolved UI language. `zh` is kept as the fast path for the existing
    // two-language call sites; setLang() is the only way to change either.
    bool zh = true;
    envh::i18n::Lang lang = envh::i18n::Lang::ZhCN;
    void setLang(envh::i18n::Lang l)
    {
        lang = l;
        zh = envh::i18n::chinese(l);
    }
    bool dark = false;
    bool writable = true; // hosts writability probe at open time
    bool systemWritable = false; // env: HKLM writable (elevated plugin)
    bool elevated = false; // the plugin process runs with an elevated token
};

} // namespace editorwin

#endif // _WIN32
