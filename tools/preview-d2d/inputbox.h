// 自绘 InputBox — 1:1 复刻 tools/preview/loading_demo.cpp:656。
// 选区 anchor + Ctrl+A/C/V/X + Shift+方向 + Backspace/Del 删选区。
// bounds / hit 改用 LayoutRect（D2D 这边的简单 4-field 矩形）。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <algorithm>

#include "anim.h"
#include "hit.h"

namespace launcher::d2d {

struct InputBox {
    std::wstring text;
    int  cursor{0};
    int  sel_anchor{-1};       // -1 = 无选区
    bool password{false};
    LayoutRect bounds{};
    Tween float_t;             // floating label 动画值

    bool hasSelection() const { return sel_anchor >= 0 && sel_anchor != cursor; }
    int  selStart() const { return (std::min)(sel_anchor, cursor); }
    int  selEnd()   const { return (std::max)(sel_anchor, cursor); }
    void clearSel() { sel_anchor = -1; }
    void selectAll() { sel_anchor = 0; cursor = (int)text.size(); }

    void deleteSelection() {
        if (!hasSelection()) return;
        int s = selStart(), e = selEnd();
        text.erase(s, e - s);
        cursor = s;
        clearSel();
    }
    void replaceSelection(const std::wstring& with) {
        deleteSelection();
        text.insert(cursor, with);
        cursor += (int)with.size();
    }

    void copyToClipboard(HWND hwnd) {
        if (!hasSelection() || password) return;
        std::wstring s = text.substr(selStart(), selEnd() - selStart());
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        size_t bytes = (s.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            memcpy(GlobalLock(h), s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    void pasteFromClipboard(HWND hwnd) {
        if (!OpenClipboard(hwnd)) return;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                std::wstring in = p;
                std::wstring filt;
                filt.reserve(in.size());
                for (wchar_t c : in) if (c != L'\r') filt.push_back(c);
                replaceSelection(filt);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    void cutToClipboard(HWND hwnd) {
        if (!hasSelection() || password) return;
        copyToClipboard(hwnd);
        deleteSelection();
    }

    // c = WM_CHAR；ctrl = Ctrl 当前是否按下；返回 true = 已处理。
    bool onChar(wchar_t c, bool ctrl, HWND hwnd) {
        if (ctrl) {
            if (c == 0x01) { selectAll(); return true; }
            if (c == 0x03) { copyToClipboard(hwnd); return true; }
            if (c == 0x16) { pasteFromClipboard(hwnd); return true; }
            if (c == 0x18) { cutToClipboard(hwnd); return true; }
            if (c == 0x1A) return true;       // Ctrl+Z (TODO)
            return true;
        }
        if (c == 0x08) {
            if (hasSelection()) deleteSelection();
            else if (cursor > 0) { text.erase(cursor - 1, 1); cursor--; }
            return true;
        }
        if (c == 0x09 || c == 0x1B) return false;   // Tab/Esc 上层处理
        if (c == 0x0A || c == 0x0D) return false;   // Enter 上层
        if (c >= 0x20) {
            replaceSelection(std::wstring(1, c));
            return true;
        }
        return false;
    }

    void onKey(int vk, bool shift, bool /*ctrl*/) {
        bool moved = false;
        if (vk == VK_LEFT) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            if (cursor > 0) { cursor--; moved = true; }
            if (!shift) clearSel();
        } else if (vk == VK_RIGHT) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            if (cursor < (int)text.size()) { cursor++; moved = true; }
            if (!shift) clearSel();
        } else if (vk == VK_HOME) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            cursor = 0; moved = true;
            if (!shift) clearSel();
        } else if (vk == VK_END) {
            if (shift && sel_anchor < 0) sel_anchor = cursor;
            cursor = (int)text.size(); moved = true;
            if (!shift) clearSel();
        } else if (vk == VK_DELETE) {
            if (hasSelection()) deleteSelection();
            else if (cursor < (int)text.size()) text.erase(cursor, 1);
        }
        (void)moved;
    }

    bool hit(POINT p) const { return bounds.contains(p); }

    std::wstring display() const {
        if (!password) return text;
        return std::wstring(text.size(), L'•');
    }
    std::wstring displaySlice(int from, int to) const {
        from = (std::max)(0, (std::min)(from, (int)text.size()));
        to   = (std::max)(0, (std::min)(to,   (int)text.size()));
        if (from >= to) return L"";
        if (!password) return text.substr(from, to - from);
        return std::wstring(to - from, L'•');
    }
};

}  // namespace launcher::d2d
