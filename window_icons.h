#pragma once
#include <windows.h>

// Own the two window icons independently of the registered window class.
// Call update on the GUI thread and clear only after the window is destroyed.
class WindowIcons {
public:
    WindowIcons() = default;
    WindowIcons(const WindowIcons&) = delete;
    WindowIcons& operator=(const WindowIcons&) = delete;
    ~WindowIcons() { clear(); }

    void update(HWND window, UINT dpi = 0)
    {
        if (!dpi) dpi = GetDpiForWindow(window);
        if (!dpi) dpi = 96;
        replace(window, ICON_BIG, SM_CXICON, SM_CYICON, dpi, large_);
        replace(window, ICON_SMALL, SM_CXSMICON, SM_CYSMICON, dpi, small_);
    }

    void clear()
    {
        if (large_) DestroyIcon(large_);
        if (small_) DestroyIcon(small_);
        large_ = small_ = nullptr;
    }

private:
    static void replace(HWND window, WPARAM kind, int cx, int cy, UINT dpi, HICON& owned)
    {
        // IDI_ICON1 is a string resource name, not numeric resource ID 1.
        const auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            L"IDI_ICON1", IMAGE_ICON, GetSystemMetricsForDpi(cx, dpi),
            GetSystemMetricsForDpi(cy, dpi), 0));
        if (!icon) return; // Keep the previous icon if allocation fails.
        SendMessageW(window, WM_SETICON, kind, reinterpret_cast<LPARAM>(icon));
        if (owned) DestroyIcon(owned);
        owned = icon;
    }
    HICON large_ = nullptr;
    HICON small_ = nullptr;
};
