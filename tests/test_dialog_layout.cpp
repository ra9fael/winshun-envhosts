// Regression: measuring a dialog before creating its body control must give
// the same geometry as measuring afterwards, including at larger text scales.
#include "editor_dialog.h"
#include <iostream>

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
    };
    for (int dpi : {96, 144, 192}) {
        for (int width : {420, 620}) {
            for (bool english : {false, true}) {
                for (bool detail : {false, true}) {
                    envh::dlg::State st;
                    st.ui = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD,
                        0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
                    st.mono = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL,
                        0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Consolas");
                    st.spec.body = english
                        ? L"Found 1 unsaved change(s) from last time.\n\nYes: restore them (Ctrl+S still required)\nNo: discard the draft"
                        : L"发现上次未保存的修改（1 项）。\n\n是：恢复这些修改（仍需 Ctrl+S 保存）\n否：丢弃草稿";
                    if (detail) st.spec.detail = L"LANG: C -> sample\nPath: C:\\alpha;C:\\beta";
                    st.spec.buttons = {{english ? L"Discard" : L"丢弃", IDNO, envh::dlg::kPlain},
                                       {english ? L"Restore" : L"恢复修改", IDYES, envh::dlg::kPrimary}};
                    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                        0, 0, width, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                    check(window != nullptr, "hidden fixture window exists");
                    if (!window) continue;
                    RECT body {}, block {};
                    std::vector<RECT> buttons;
                    const int before = envh::dlg::contentHeight(window, st, width, body, block, buttons, false);
                    HWND ctl = CreateWindowExW(0, L"STATIC", st.spec.body.c_str(), WS_CHILD,
                        0, 0, 10, 10, window, reinterpret_cast<HMENU>(0xFFFF), GetModuleHandleW(nullptr), nullptr);
                    check(ctl != nullptr, "body fixture exists");
                    const int after = envh::dlg::contentHeight(window, st, width, body, block, buttons, true);
                    check(before == after, "height independent of child creation");
                    for (const auto& button : buttons) {
                        check(button.left >= 0 && button.right <= width && button.bottom <= before,
                              "complete button lies inside the initially sized window");
                        check(button.top >= std::max(body.bottom, block.bottom) + 20,
                              "body/detail leaves button clearance");
                    }
                    DestroyWindow(window);
                }
            }
        }
    }
    std::cout << checks - failures << '/' << checks << " dialog layout checks passed\n";
    return failures ? 1 : 0;
}
