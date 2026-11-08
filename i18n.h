// i18n.h - locale resolution for the two supported UI languages.
//
// WinShun resolves its own language to "zh" or "en" (App.h: m_language, and
// Manifest.cpp Text::pick), and plugin manifests only carry {zh, en}. A plugin
// can therefore follow the host, or pin one of the two languages from its own
// settings; there is no third host locale to react to. All user-visible text in
// this plugin is a zh/en pair selected at draw time, so a language change is
// applied to an already open window without rebuilding the model.
//
// Code and comments are English.
#pragma once

#include <string>

namespace envh {
namespace i18n {

enum class Lang { ZhCN, EnUS };

// The host sends "zh" / "en"; accept the common BCP-47 spellings too.
inline Lang parse(const std::string& s)
{
    if (s.size() >= 2 && (s[0] == 'e' || s[0] == 'E') && (s[1] == 'n' || s[1] == 'N'))
        return Lang::EnUS;
    return Lang::ZhCN;
}

// `override` is the plugin setting: "zh-CN" / "en-US" pin a language, anything
// else (including "auto" and an empty string) follows the host locale.
inline Lang resolve(const std::string& host, const std::string& overrideLang)
{
    if (overrideLang == "en-US")
        return Lang::EnUS;
    if (overrideLang == "zh-CN")
        return Lang::ZhCN;
    return parse(host);
}

inline bool chinese(Lang l) { return l == Lang::ZhCN; }

inline const char* name(Lang l) { return chinese(l) ? "zh-CN" : "en-US"; }

} // namespace i18n
} // namespace envh
