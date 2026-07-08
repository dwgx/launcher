// 多行文本编辑核心 — chat composer 用(与登录页单行 InputBox 隔离,互不影响)。
// 逻辑模型:text 是含 '\n' 的扁平串;cursor/sel_anchor 是字符偏移。
// 软换行(按宽度折行)是"显示层",由 paint 侧用 DirectWrite 测量后叠加;本类只管
// 逻辑行(\n 分隔)、光标/选区移动、编辑、撤销栈。
//
// 快捷键(onChar/onKey):
//   Enter        -> 交上层(发送);Shift+Enter -> 插入 '\n'
//   Ctrl+A/C/V/X -> 全选/复制/粘贴/剪切
//   Ctrl+Z       -> 撤销;Ctrl+Shift+Z / Ctrl+Y -> 重做
//   ←→↑↓/Home/End(+Shift 选区)、Backspace/Delete
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>
#include <vector>
#include <algorithm>

#include "hit.h"    // LayoutRect(bounds 命中用)

namespace launcher::d2d {

struct MultilineEdit {
    std::wstring text;
    int  cursor{0};
    int  sel_anchor{-1};              // -1 = 无选区
    LayoutRect bounds{};              // 命中矩形(paint 每帧写,onMouseLDown 读)

    // 撤销/重做:快照式(composer 文本量小,直接存 {text,cursor} 最简单可靠)。
    struct Snap { std::wstring text; int cursor; };
    std::vector<Snap> undo_stack;
    std::vector<Snap> redo_stack;
    bool  coalescing{false};          // 连续普通输入合并成一个 undo 单元

    // ---- 选区 ----
    bool hasSelection() const { return sel_anchor >= 0 && sel_anchor != cursor; }
    int  selStart() const { return (std::min)(sel_anchor, cursor); }
    int  selEnd()   const { return (std::max)(sel_anchor, cursor); }
    void clearSel() { sel_anchor = -1; }
    void selectAll() { sel_anchor = 0; cursor = (int)text.size(); }

    // ---- 撤销栈 ----
    void pushUndo() {
        undo_stack.push_back({ text, cursor });
        if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
    }
    // 成组:连续字符输入合并;删除/粘贴/换行各自成组(先 breakCoalesce 再 pushUndo)。
    void breakCoalesce() { coalescing = false; }
    void undo() {
        if (undo_stack.empty()) return;
        redo_stack.push_back({ text, cursor });
        Snap s = undo_stack.back(); undo_stack.pop_back();
        text = std::move(s.text); cursor = s.cursor; clearSel(); coalescing = false;
    }
    void redo() {
        if (redo_stack.empty()) return;
        undo_stack.push_back({ text, cursor });
        Snap s = redo_stack.back(); redo_stack.pop_back();
        text = std::move(s.text); cursor = s.cursor; clearSel(); coalescing = false;
    }

    // ---- 编辑 ----
    void deleteSelection() {
        if (!hasSelection()) return;
        int s = selStart(), e = selEnd();
        text.erase(s, e - s);
        cursor = s;
        clearSel();
    }
    // 插入(替换选区)。coalesce=true 时与上一次普通输入合并为同一 undo 单元。
    void insert(const std::wstring& with, bool coalesce) {
        if (!coalesce || !coalescing) { pushUndo(); coalescing = coalesce; }
        deleteSelection();
        text.insert(cursor, with);
        cursor += (int)with.size();
    }

    // ---- 剪贴板 ----
    void copyToClipboard(HWND hwnd) {
        if (!hasSelection()) return;
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
                std::wstring in = p, filt;
                filt.reserve(in.size());
                for (wchar_t c : in) if (c != L'\r') filt.push_back(c);  // 规整 CRLF->LF
                breakCoalesce();
                insert(filt, /*coalesce=*/false);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    void cutToClipboard(HWND hwnd) {
        if (!hasSelection()) return;
        copyToClipboard(hwnd);
        breakCoalesce(); pushUndo(); coalescing = false;
        deleteSelection();
    }

    // WM_CHAR。返回值:0=未处理,1=已处理,2=请求发送(Enter 无 Shift)。
    // shift 用于区分 Enter 发送 vs Shift+Enter 换行(WM_CHAR 里 Enter=0x0D)。
    int onChar(wchar_t c, bool ctrl, bool shift, HWND hwnd) {
        if (ctrl) {
            if (c == 0x01) { selectAll(); return 1; }               // Ctrl+A
            if (c == 0x03) { copyToClipboard(hwnd); return 1; }     // Ctrl+C
            if (c == 0x16) { pasteFromClipboard(hwnd); return 1; }  // Ctrl+V
            if (c == 0x18) { cutToClipboard(hwnd); return 1; }      // Ctrl+X
            if (c == 0x1A) { undo(); return 1; }                    // Ctrl+Z
            if (c == 0x19) { redo(); return 1; }                    // Ctrl+Y
            return 1;   // 其余 ctrl 组合吞掉(Ctrl+Shift+Z 走 onKey)
        }
        if (c == 0x08) {   // Backspace
            breakCoalesce();
            if (hasSelection()) { pushUndo(); coalescing = false; deleteSelection(); }
            else if (cursor > 0) { pushUndo(); coalescing = false; text.erase(cursor - 1, 1); cursor--; }
            return 1;
        }
        if (c == 0x1B) return 0;                 // Esc 交上层
        if (c == 0x09) { insert(L"\t", true); return 1; }  // Tab -> 制表(可选)
        if (c == 0x0A || c == 0x0D) {            // Enter
            if (shift) { breakCoalesce(); insert(L"\n", false); return 1; }  // Shift+Enter 换行
            return 2;                            // Enter -> 请求发送
        }
        if (c >= 0x20) { insert(std::wstring(1, c), /*coalesce=*/true); return 1; }
        return 0;
    }

    // ---- 逻辑行辅助(\n 分隔;软换行在 paint 侧另算)----
    // 返回 off 所在逻辑行的起始偏移(该行首字符),即上一个 '\n' 之后。
    int lineStart(int off) const {
        int i = (std::min)(off, (int)text.size());
        while (i > 0 && text[i - 1] != L'\n') --i;
        return i;
    }
    // 返回 off 所在逻辑行的结束偏移(行尾 '\n' 之前,或串尾)。
    int lineEnd(int off) const {
        int i = (std::min)(off, (int)text.size());
        while (i < (int)text.size() && text[i] != L'\n') ++i;
        return i;
    }
    int columnOf(int off) const { return off - lineStart(off); }

    // 上下移动:保持列(col)。到相邻逻辑行,列钳到该行长度。
    int offsetLineUp(int off) const {
        int ls = lineStart(off);
        if (ls == 0) return 0;                 // 已在首行
        int col = off - ls;
        int prevEnd = ls - 1;                  // 上一行的 '\n' 位置
        int prevStart = lineStart(prevEnd);
        int prevLen = prevEnd - prevStart;
        return prevStart + (std::min)(col, prevLen);
    }
    int offsetLineDown(int off) const {
        int le = lineEnd(off);
        if (le >= (int)text.size()) return (int)text.size();  // 已在末行
        int ls = lineStart(off);
        int col = off - ls;
        int nextStart = le + 1;                // 跳过 '\n'
        int nextEnd = lineEnd(nextStart);
        int nextLen = nextEnd - nextStart;
        return nextStart + (std::min)(col, nextLen);
    }

    // WM_KEYDOWN。返回 true=已处理。多行:↑↓ 跨逻辑行;Home/End 行内。
    bool onKey(int vk, bool shift, bool ctrl) {
        auto beginMove = [&](){ if (shift && sel_anchor < 0) sel_anchor = cursor;
                                if (!shift) {} };
        auto endMove   = [&](){ if (!shift) clearSel(); };
        if (vk == VK_LEFT)  { beginMove(); if (cursor > 0) cursor--; endMove(); return true; }
        if (vk == VK_RIGHT) { beginMove(); if (cursor < (int)text.size()) cursor++; endMove(); return true; }
        if (vk == VK_UP)    { beginMove(); cursor = offsetLineUp(cursor);   endMove(); return true; }
        if (vk == VK_DOWN)  { beginMove(); cursor = offsetLineDown(cursor); endMove(); return true; }
        if (vk == VK_HOME)  { beginMove(); cursor = ctrl ? 0 : lineStart(cursor); endMove(); return true; }
        if (vk == VK_END)   { beginMove(); cursor = ctrl ? (int)text.size() : lineEnd(cursor); endMove(); return true; }
        if (vk == VK_DELETE) {
            breakCoalesce();
            if (hasSelection()) { pushUndo(); coalescing = false; deleteSelection(); }
            else if (cursor < (int)text.size()) { pushUndo(); coalescing = false; text.erase(cursor, 1); }
            return true;
        }
        return false;
    }

    // 逻辑行数(用于自动增高的下限;真实显示行数含软换行由 paint 算)。
    int logicalLineCount() const {
        int n = 1;
        for (wchar_t c : text) if (c == L'\n') ++n;
        return n;
    }

    void reset() { text.clear(); cursor = 0; clearSel(); undo_stack.clear(); redo_stack.clear(); coalescing = false; }

    // ---- InputBox 兼容层(chat_paint / chat 迁移用,最小改动)----
    // composer 非密码框,displaySlice = 子串。
    std::wstring displaySlice(int from, int to) const {
        from = (std::max)(0, (std::min)(from, (int)text.size()));
        to   = (std::max)(0, (std::min)(to,   (int)text.size()));
        if (from >= to) return L"";
        return text.substr(from, to - from);
    }
    // 提及插入等:替换选区(作为独立 undo 单元)。
    void replaceSelection(const std::wstring& with) {
        breakCoalesce();
        insert(with, /*coalesce=*/false);
    }
    bool hit(POINT p) const { return bounds.contains(p); }
};

}  // namespace launcher::d2d
