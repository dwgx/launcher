// Chat view 实现 — 见 chat.h。GDI+ 等价 tools/preview/chat_view.inl。

#include "chat.h"
#include "chat_internal.h"
#include "icons.h"
#include "palette.h"
#include "user_state.h"
#include "net.h"
#include "fetch.h"
#include "hit.h"
#include "stages.h"
#include "sticker.h"
#include "modals.h"
#include "render/primitives.h"
#include "toast.h"
#include "i18n.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <wincodec.h>
#include <wrl/client.h>
#include <shellapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

namespace launcher::d2d::chat {

void switchChannel(const std::wstring& slug) {
    g_active = slug;
    g_focus_composer = false;
    g_composer_drag = ComposerDrag{};
    if (g_picker_open) setPickerOpen(false);
    if (g_pending_reply.active && g_pending_reply.slug != slug) {
        g_pending_reply = PendingReply{};
        g_pending_mentions.clear();
    }
    if (slug == L"announcements") {
        rebuildAnnouncementStream();
        markAnnouncementsReadLocal(false);
    }
    std::string chat_id;
    for (auto& c : g_channels) {
        if (c.slug == slug) { chat_id = c.id; break; }
    }
    auto& hist_state = g_history_state[slug];
    if (!chat_id.empty()
        && (hist_state == HistoryLoadState::NotStarted
            || hist_state == HistoryLoadState::Failed)) {
        fetchHistory(GetActiveWindow(), slug);
    }
    // 切到这个频道时不重置 scroll — 保留之前的位置（用户切到设置再切回来还在原位）
    // 但首次进入会通过 ChatScroll::initialized = false 自动 stick to bottom
}

void onWheel(int delta) {
    if (g_picker_open && g_picker_rect.contains(g_mouse)) {
        if (g_picker_tab == 0) {
            float max_scroll = (std::max)(0.0f, g_emoji_total_h_last - g_emoji_grid_h_last);
            g_emoji_scroll_y = clampf(g_emoji_scroll_y - (float)delta * 0.5f, 0.0f, max_scroll);
        } else {
            float max_scroll = (std::max)(0.0f, g_pack_total_h_last - g_pack_grid_h_last);
            g_pack_scroll_y = clampf(g_pack_scroll_y - (float)delta * 0.5f, 0.0f, max_scroll);
        }
        return;
    }
    auto& sc = g_scroll[g_active];
    if (sc.total_height <= sc.viewport_h) return;
    // 写 target_offset，每帧 lerp 平滑到位（避免一格 60px 跳跃感）
    // 一次 wheel notch (delta=120) → 滚 90px，连续滚自动累积
    float dy = (float)delta * 0.75f;
    sc.target_offset += dy;
    float max_off = sc.total_height - sc.viewport_h;
    if (sc.target_offset > max_off) sc.target_offset = max_off;
    if (sc.target_offset < 0) sc.target_offset = 0;
}

void appendLocalMessage(Msg msg) {
    appendOrMergeMessage(g_active, std::move(msg));
    auto& sc = g_scroll[g_active];
    sc.offset_from_bottom = 0;
    sc.target_offset = 0;
}

void retargetTopSeg() {
    // 表情 / 表情包 顶部 seg pill — 0/1
    int idx = (g_picker_tab == 0) ? 0 : 1;
    float target_x = idx * (60 + 6);     // 第二个 tab 起点 (60 宽 + 6 间隔)
    float target_w = (idx == 0) ? 60.0f : 80.0f;     // 表情 60 / 表情包 80
    if (!g_top_seg_x.started) g_top_seg_x.start(target_x, target_x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_x.to - target_x) > 0.5f)
        g_top_seg_x.start(g_top_seg_x.value(), target_x, 0.10f, 0, curve::easeOutCubic);
    if (!g_top_seg_w.started) g_top_seg_w.start(target_w, target_w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_top_seg_w.to - target_w) > 0.5f)
        g_top_seg_w.start(g_top_seg_w.value(), target_w, 0.10f, 0, curve::easeOutCubic);
}

void retargetPackTab() {
    int active = g_picker_tab - 1;     // pack idx (0-based 在 g_packs 里)
    if (active < 0) return;
    if (active >= (int)g_pack_tab_rects.size()) return;
    LayoutRect tr;
    bool found = false;
    for (auto& pt : g_pack_tab_rects) {
        if (pt.idx == active) { tr = pt.r; found = true; break; }
    }
    if (!found) return;
    if (!g_pack_tab_x.started) g_pack_tab_x.start(tr.x, tr.x, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_x.to - tr.x) > 0.5f)
        g_pack_tab_x.start(g_pack_tab_x.value(), tr.x, 0.10f, 0, curve::easeOutCubic);
    if (!g_pack_tab_w.started) g_pack_tab_w.start(tr.w, tr.w, 0.001f, 0, curve::easeOutQuint);
    else if (std::abs(g_pack_tab_w.to - tr.w) > 0.5f)
        g_pack_tab_w.start(g_pack_tab_w.value(), tr.w, 0.10f, 0, curve::easeOutCubic);
}

void setPickerTabSmooth(int tab) {
    if (tab < 0) tab = 0;
    if (g_picker_tab == tab) return;
    int old_tab = g_picker_tab;
    g_picker_tab = tab;
    g_picker_content_tab = tab;
    if (tab > 0 && tab != old_tab) g_pack_scroll_y = 0.0f;
    g_picker_scroll_drag = PickerScrollDrag{};
    g_pack_drag = PackDrag{};
    g_picker_content_t.start(0.0f, 1.0f, 0.16f, 0, curve::easeOutCubic);
    retargetTopSeg();
    if (g_picker_tab > 0) retargetPackTab();
}

// 判定一条消息是否属于当前登录用户。
// 关键修复：不再只信任 m.from==L"me" 这个在发送时写死的字面量
// （切换账号后会污染——上个账号发的消息在新账号下仍被当成"自己"靠右）。
// 而是把消息 author_key / peer_key 与当前 g_user_id 比对来动态判定。
bool isSelfMessage(const Msg& m) {
    // 当前没有登录身份时，退回旧的字面量判定（保持单账号行为不变）。
    if (g_user_id.empty()) return m.from == L"me";
    std::wstring self = utf8wHist(g_user_id);
    if (!m.author_key.empty()) return m.author_key == self;
    if (!m.peer_key.empty())   return m.peer_key == self;
    // author_key/peer_key 都为空（极少数本地构造）时才看字面量。
    return m.from == L"me";
}

// 登录成功 / 切换账号时调用：清空所有频道消息与相关派生状态，
// 防止上一个账号的消息缓存（含 from==me 标记）泄漏到新账号视图。
void resetForAccount() {
    {
        std::lock_guard<std::mutex> lk(g_streams_mtx);
        g_streams.clear();
        g_group_collapsed.clear();
        g_group_anim.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_reply_snapshots_mtx);
        g_reply_snapshots.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_peer_profile_requested_mtx);
        g_peer_profile_requested_at.clear();
    }
    g_history_state.clear();
    g_scroll.clear();
    {
        std::lock_guard<std::mutex> lk(g_pending_hist_mtx);
        g_pending_history.clear();
    }
}

void addMentionToComposer(const std::wstring& user_id, const std::wstring& label) {
    if (user_id.empty()) return;
    for (auto& m : g_pending_mentions) {
        if (m.user_id == user_id) return;
    }
    std::wstring clean = label.empty() ? user_id : label;
    for (auto& c : clean) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    }
    while (!clean.empty() && clean.front() == L' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == L' ') clean.pop_back();
    g_pending_mentions.push_back({ user_id, clean });
    std::wstring token = L"@" + clean;
    if (!g_composer.text.empty()) {
        wchar_t prev = g_composer.text.back();
        if (prev != L' ' && prev != L'\n' && prev != L'\t') token = L" " + token;
    }
    token += L" ";
    g_composer.replaceSelection(token);
    g_focus_composer = true;
}

void beginReplyToMessage(const std::wstring& slug, int64_t server_id,
                         const std::string& client_msg_id,
                         const std::wstring& author,
                         const std::wstring& author_key,
                         const std::wstring& preview) {
    g_pending_reply = PendingReply{};
    g_pending_reply.active = true;
    g_pending_reply.id = server_id;
    g_pending_reply.client_msg_id = client_msg_id;
    g_pending_reply.slug = slug.empty() ? g_active : slug;
    g_pending_reply.author = author.empty() ? L"message" : author;
    g_pending_reply.author_key = author_key;
    g_pending_reply.preview = preview.empty() ? L"[message]" : preview;
    g_focus_composer = true;
}

bool appendOrMergeMessage(const std::wstring& slug, Msg msg) {
    normalizeMsgIdentity(msg);
    fillReplySnapshot(msg);
    auto& s = streamFor(slug);
    auto& sc = g_scroll[slug];
    bool was_at_bottom = (sc.offset_from_bottom < 16.0f && sc.target_offset < 16.0f);
    for (auto& existing : s) {
        if (!sameMessageIdentityStrict(existing, msg)) continue;
        mergeServerIdentity(existing, msg);
        fillReplySnapshot(existing);
        rememberReplySnapshot(slug, existing);
        resolveLocalReplyTargets(slug);
        return false;
    }
    s.push_back(std::move(msg));
    rememberReplySnapshot(slug, s.back());
    resolveLocalReplyTargets(slug);
    if (was_at_bottom) {
        sc.offset_from_bottom = 0;
        sc.target_offset = 0;
    }
    return true;
}

void focusMessage(const std::wstring& slug, int64_t server_id) {
    if (server_id <= 0) return;
    if (!slug.empty()) switchChannel(slug);
    g_focus_target.slug = slug.empty() ? g_active : slug;
    g_focus_target.server_id = server_id;
    g_focus_target.highlight_until = stages::g_time_in_stage + 2.2f;
    g_focus_target.scroll_applied = false;
    g_focus_target.missing_reported = false;
    if (!hasMessageServerId(g_focus_target.slug, server_id)) {
        auto state = g_history_state[g_focus_target.slug];
        if (state == HistoryLoadState::Loaded || state == HistoryLoadState::Failed) {
            fetchHistory(GetActiveWindow(), g_focus_target.slug);
        } else if (state == HistoryLoadState::Exhausted) {
            g_focus_target.missing_reported = true;
        }
    }
}

Channel* activeChannel() {
    for (auto& c : g_channels) if (c.slug == g_active) return &c;
    return g_channels.empty() ? nullptr : &g_channels.front();
}

std::string activeChatId() {
    if (g_channels.empty()) return {};
    if (auto* ch = activeChannel()) return ch->id;
    return {};
}

bool currentUserCanWriteRestrictedChannel() {
    if (g_user.is_admin || g_bootstrap_is_admin) return true;
    const std::wstring& role = !g_bootstrap_role.empty() ? g_bootstrap_role : g_user.role;
    return role == L"admin" || role == L"owner" || role == L"super_admin";
}

bool canWriteChannel(const Channel* ch) {
    if (!ch || ch->id.empty()) return false;
    if (ch->is_locked || ch->is_readonly
        || ch->write_policy == "locked"
        || ch->write_policy == "readonly") {
        return false;
    }
    if (ch->write_policy == "admin_only") return currentUserCanWriteRestrictedChannel();
    if (ch->write_policy == "role_only") {
        if (currentUserCanWriteRestrictedChannel()) return true;
        if (ch->allowed_role.empty()) return false;
        const std::wstring role = !g_bootstrap_role.empty() ? g_bootstrap_role : g_user.role;
        return role == utf8wHist(ch->allowed_role);
    }
    if (ch->write_policy == "subscriber_only" || ch->requires_subscription) {
        if (currentUserCanWriteRestrictedChannel()) return true;
        return g_user.subscribed;
    }
    if (ch->write_policy == "min_level") {
        if (currentUserCanWriteRestrictedChannel()) return true;
        return g_user.level >= ch->min_level;
    }
    if (!ch->write_policy.empty()) return true;
    if (ch->write_role <= 0) return true;
    return currentUserCanWriteRestrictedChannel();
}

bool canWriteActiveChannel() {
    return canWriteChannel(activeChannel());
}

std::wstring activeWriteBlockedMessage() {
    auto* ch = activeChannel();
    if (!ch || ch->id.empty()) return trW("channel.blocked.not_loaded");
    if (ch->is_locked || ch->write_policy == "locked") return trW("channel.blocked.locked");
    if (ch->is_readonly || ch->write_policy == "readonly") return trW("channel.blocked.readonly");
    if ((ch->write_policy == "admin_only" || (ch->write_policy.empty() && ch->write_role > 0))
        && !currentUserCanWriteRestrictedChannel()) {
        return trW("channel.blocked.admin_only");
    }
    if (ch->write_policy == "role_only") return trW("channel.blocked.role_only");
    if (ch->write_policy == "subscriber_only" || ch->requires_subscription) return trW("channel.blocked.subscriber_only");
    if (ch->write_policy == "min_level") return trW("channel.blocked.min_level");
    return trW("channel.blocked.default");
}

bool requireActiveChannelWrite() {
    if (canWriteActiveChannel()) return true;
    toast::show(activeWriteBlockedMessage());
    return false;
}

std::wstring normalizeSendErrorText(std::wstring err) {
    if (err.find(L"channel is admin only") != std::wstring::npos) {
        return trW("channel.blocked.admin_only");
    }
    if (err.find(L"channel is read only") != std::wstring::npos
        || err.find(L"channel is readonly") != std::wstring::npos
        || err.find(L"channel is locked") != std::wstring::npos) {
        return trW("channel.blocked.default");
    }
    return err;
}

void tick(float dt) {
    g_picker_t.tick(dt);
    g_picker_content_t.tick(dt);
    g_popup_t.tick(dt);
    g_top_seg_x.tick(dt); g_top_seg_w.tick(dt);
    g_pack_tab_x.tick(dt); g_pack_tab_w.tick(dt);
    g_picker_sel_x.tick(dt); g_picker_sel_y.tick(dt);
    for (auto& kv : g_group_anim) kv.second.tick(dt);
}

// 把剪贴板 DIB(BITMAPINFO + 像素)用 WIC 编码成临时 PNG,返回文件路径(失败空)。
// 走 HBITMAP → WIC CreateBitmapFromHBITMAP 路线,兼容各种位深/方向的 DIB。
static std::wstring encodeDibToPngTemp(IWICImagingFactory* wic, void* dib, SIZE_T /*sz*/) {
    using Microsoft::WRL::ComPtr;
    BITMAPINFO* bi = (BITMAPINFO*)dib;
    int w = bi->bmiHeader.biWidth;
    int h = std::abs(bi->bmiHeader.biHeight);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return L"";
    // DIB 像素数据紧跟在头(+调色板/掩码)之后。用 CreateDIBitmap 建 HBITMAP。
    HDC hdc = GetDC(nullptr);
    const BYTE* bits = (const BYTE*)dib + bi->bmiHeader.biSize;
    // 有掩码(BI_BITFIELDS)时像素前还有 3 个 DWORD;调色板同理。用 GetDIBits 更稳:
    HBITMAP hbmp = CreateDIBitmap(hdc, &bi->bmiHeader, CBM_INIT, bits, bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (!hbmp) return L"";

    ComPtr<IWICBitmap> wbmp;
    HRESULT hr = wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wbmp);
    DeleteObject(hbmp);
    if (FAILED(hr) || !wbmp) return L"";

    // 输出路径:%LOCALAPPDATA%\Launcher\paste\<time>.png
    std::wstring dir = fetch::mediaCacheDir(L"paste");
    wchar_t name[64];
    swprintf(name, 64, L"paste_%llu.png", (unsigned long long)GetTickCount64());
    std::wstring outPath = dir + name;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE)))
        return L"";
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return L"";
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)))
        return L"";
    if (FAILED(frame->WriteSource(wbmp.Get(), nullptr))) return L"";
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return L"";
    return outPath;
}

// Ctrl+V:剪贴板若有图片(CF_DIB)或复制的文件(CF_HDROP)→ 发媒体,返回 true。
// CF_HDROP:逐个文件走 appendMedia(和拖拽一致)。
// CF_DIB:用 WIC 把位图编码成临时 PNG(上传管线不支持 bmp),再 appendMedia。
static bool tryPathTextToAttachment(const std::wstring& text);   // 前置声明
bool tryPasteImageFromClipboard(HWND hwnd, IWICImagingFactory* wic) {
    if (!requireActiveChannelWrite()) return false;
    // 1) 复制的文件(资源管理器 Ctrl+C 文件)
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (!OpenClipboard(hwnd)) return false;
        bool handled = false;
        HANDLE h = GetClipboardData(CF_HDROP);
        if (h) {
            HDROP drop = (HDROP)h;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, i, path, MAX_PATH)) { if (addComposerAttachment(path)) handled = true; }
            }
        }
        CloseClipboard();
        if (handled) return true;
    }
    // 2) 位图(截图/图片编辑器 Ctrl+C)→ 编码 PNG 到临时文件
    if (wic && (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB))) {
        if (!OpenClipboard(hwnd)) return false;
        bool ok = false;
        UINT fmt = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
        HANDLE h = GetClipboardData(fmt);
        if (h) {
            void* dib = GlobalLock(h);
            SIZE_T sz = GlobalSize(h);
            if (dib && sz > sizeof(BITMAPINFOHEADER)) {
                std::wstring outPath = encodeDibToPngTemp(wic, dib, sz);
                if (!outPath.empty()) { ok = addComposerAttachment(outPath); }
            }
            if (dib) GlobalUnlock(h);
        }
        CloseClipboard();
        if (ok) return true;
    }
    // 3) 剪贴板文本恰好是本地图片路径 → 也变 chip(而非粘成路径文字)
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (!OpenClipboard(hwnd)) return false;
        bool ok = false;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) { ok = tryPathTextToAttachment(p); GlobalUnlock(h); }
        }
        CloseClipboard();
        if (ok) return true;   // 是路径 → 已入暂存区,吞掉粘贴
    }
    return false;
}

// 若 text 是"单个本地图片/媒体文件路径"→ 加入附件暂存区返回 true;否则 false。
// 粘贴文本时用:粘进来的若是图片路径,直接变 chip 而非路径文字。
static bool tryPathTextToAttachment(const std::wstring& text) {
    // trim 首尾空白 + 去掉可能的引号
    std::wstring p = text;
    size_t a = p.find_first_not_of(L" \t\r\n\"");
    size_t b = p.find_last_not_of(L" \t\r\n\"");
    if (a == std::wstring::npos) return false;
    p = p.substr(a, b - a + 1);
    if (p.find(L'\n') != std::wstring::npos) return false;   // 多行不当路径
    auto dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = p.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    // 只认上传管线支持的扩展(不含 bmp — 上传不支持)
    bool ok = ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp"
           || ext == L".gif" || ext == L".mp4" || ext == L".webm";
    if (!ok) return false;
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return false;  // 文件须存在
    // 变成附件 chip(而非直接发路径文本)
    return addComposerAttachment(p);
}

// 按扩展名归类可上传媒体;不支持返回 nullptr。
static const char* mediaKindForPath(const std::wstring& path) {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return nullptr;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp") return "image";
    if (ext == L".gif") return "gif";
    if (ext == L".mp4" || ext == L".webm") return "video";
    return nullptr;
}

// 把图片/媒体加入输入框附件暂存区(变缩略图 chip),不立即发送。返回是否加入。
bool addComposerAttachment(const std::wstring& path) {
    if (!requireActiveChannelWrite()) return false;
    const char* kind = mediaKindForPath(path);
    if (!kind) return false;
    if (g_composer_attachments.size() >= 9) return false;   // 上限,避免刷屏
    g_composer_attachments.push_back({ path, kind });
    g_focus_composer = true;
    return true;
}

void appendMedia(const std::wstring& path) {
    if (!requireActiveChannelWrite()) return;
    Msg m;
    std::wstring p = path;
    auto dot = p.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? p.substr(dot) : L"";
    for (auto& c : ext) c = (wchar_t)towlower(c);
    const char* kind = "text";
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp") {
        m.kind = MsgKind::Image;
        kind = "image";
    } else if (ext == L".gif") {
        m.kind = MsgKind::Gif;
        kind = "gif";
    } else if (ext == L".mp4" || ext == L".webm" || ext == L".mov" || ext == L".avi" || ext == L".mkv") {
        m.kind = MsgKind::Video;
        kind = "video";
    } else {
        m.kind = MsgKind::Text;
        m.body = trW("chat.file_msg");
        if (auto p = m.body.find(L"{path}"); p != std::wstring::npos)
            m.body.replace(p, 6, path);
        m.from = L"me"; m.author = g_user.nickname; m.author_key = selfAuthorKey();
        m.status = L"online"; m.time = localTimeText();
        m.client_msg_id = makeClientMsgId();
        appendLocalMessage(std::move(m));
        return;
    }
    m.from = L"me";
    m.author = g_user.nickname;
    m.author_key = selfAuthorKey();
    m.status = L"online";
    m.body = path;
    m.time = localTimeText();
    m.client_msg_id = makeClientMsgId();
    m.send_state = MsgSendState::Pending;
    std::string client_msg_id = m.client_msg_id;
    appendLocalMessage(std::move(m));
    sendChatMessage(GetActiveWindow(), path, kind, client_msg_id);
}

// ============== 事件 ==============
// picker 的"点外关闭"已由统一浮层栈(g_overlays)接管;本函数只在栈把 picker 内部
// 点击 defer 回来时、或 picker 未开时被 WndProc 调用。职责纯化为:composer 聚焦/
// 选区拖拽起点、pack tab 拖拽起点、以及一般 view 控件的 dispatchClick。
bool onMouseLDown(HWND /*hwnd*/, POINT dip) {
    bool clicked_picker = g_picker_open && g_picker_rect.contains(dip);
    bool clicked_emoji_button = g_emoji_button_rect.contains(dip);
    bool clicked_composer = g_composer.bounds.contains(dip);
    if (clicked_composer && canWriteActiveChannel()) {
        g_focus_composer = true;
        int pos = cursorFromComposerPoint((float)dip.x);
        g_composer.cursor = pos;
        g_composer.sel_anchor = pos;
        g_composer_drag.active = true;
        g_composer_drag.anchor_cursor = pos;
    } else {
        g_composer_drag.active = false;
    }
    // pack tab 拖拽起点 — 在 picker 打开 + 表情包 tab + 命中某个 tab 时初始化拖动状态
    g_pack_drag = PackDrag{};
    if (g_picker_open && g_picker_tab > 0) {
        for (auto& pt : g_pack_tab_rects) {
            if (pt.r.contains(dip)) {
                g_pack_drag.from = pt.idx;
                g_pack_drag.over = pt.idx;
                g_pack_drag.anchor_dx = (float)(dip.x - pt.r.x);
                g_pack_drag.start_x = (float)dip.x;
                g_pack_drag.moved = false;
                break;
            }
        }
    }
    bool consumed = dispatchClick(dip);
    if (g_focus_composer && !clicked_composer && !clicked_picker && !clicked_emoji_button) {
        g_focus_composer = false;
    }
    if (g_composer_drag.active && !g_composer.hasSelection()) {
        g_composer.clearSel();
    }
    return consumed;
}

bool onMouseMove(HWND /*hwnd*/, POINT dip) {
    if (!g_composer_drag.active || !g_mouse_pressed) return false;
    g_focus_composer = true;
    if (g_composer.sel_anchor < 0) {
        g_composer.sel_anchor = g_composer_drag.anchor_cursor;
    }
    g_composer.cursor = cursorFromComposerPoint((float)dip.x);
    return true;
}

bool pointInStream(POINT dip) {
    return g_chat_stream_rect.contains(dip);
}

bool onMouseRDown(HWND hwnd, POINT dip) {
    if (modal::hasBlockingModalOpen()) return true;
    // 优先：头像右键 → 用户菜单（主页 / @ / 管理）
    for (auto it = g_avatar_hits.rbegin(); it != g_avatar_hits.rend(); ++it) {
        if (it->rect.contains(dip)) {
            std::wstring label = it->peer_key;
            fetch::PeerProfile peer = fetch::peerProfileCached(it->peer_key);
            if (peer.loaded && peer.err.empty()) {
                if (!peer.nickname.empty()) label = peer.nickname;
                else if (!peer.username.empty()) label = peer.username;
                else if (!peer.uid.empty()) label = peer.uid;
            } else if (it->peer_key == selfAuthorKey()) {
                label = g_user.nickname.empty() ? g_user.username : g_user.nickname;
            }
            if (g_picker_open) setPickerOpen(false);
            modal::openUserContextMenu(dip, it->peer_key, label);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
    }
    if (g_active == L"announcements" && g_announcements_stream_dirty) {
        rebuildAnnouncementStream();
    }
    auto& msgs = streamFor(g_active);
    auto open_msg_menu = [&](const std::vector<MsgHit>& hits) -> bool {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            if (!it->rect.contains(dip)) continue;
            int idx = it->idx;
            if (idx < 0 || idx >= (int)msgs.size()) return true;
            const Msg& m = msgs[idx];
            if (m.kind == MsgKind::System || m.kind == MsgKind::DayDivider) return true;
            if (g_picker_open) setPickerOpen(false);
            modal::openMsgContextMenu(dip, idx);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        return false;
    };
    if (open_msg_menu(g_msg_row_hits)) return true;
    if (open_msg_menu(g_msg_hits)) return true;
    modal::closeContextMenus();
    return true;
}

bool onMouseLUp(HWND /*hwnd*/, POINT /*dip*/) {
    // pack 拖拽抬起 — 真正提交顺序由 paintPicker 在拖动时即时本地排过；这里只发后端
    if (g_pack_drag.from >= 0 && g_pack_drag.moved) {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> lk(sticker::g_packs_mtx);
            for (auto& p : sticker::g_packs) {
                if (!p.id.empty()) ids.push_back(p.id);
            }
        }
        sticker::reorderPacks(GetActiveWindow(), ids);
    }
    g_pack_drag = PackDrag{};
    g_composer_drag.active = false;
    g_scroll_drag.active = false;
    g_picker_scroll_drag.active = false;
    return false;
}

// 发送/插入一个 emoji glyph(键盘 Enter 与鼠标点击共用)。
void sendEmojiGlyph(const std::wstring& s) {
    if (g_react_target.active) {
        reactToMessage(GetActiveWindow(), g_react_target.slug,
                       g_react_target.server_id, s, /*remove=*/false);
        g_react_target.active = false;
        setPickerOpen(false);
    } else {
        g_composer.replaceSelection(s);
        g_focus_composer = true;
        // 不自动关 picker — 用户可能连续选
    }
}

// picker 顶部搜索框打字 → 过滤 emoji。仅 emoji tab + 搜索框聚焦时消费(防止漏进 composer)。
// 返回 true=已消费(printable/退格/Enter/Tab 都吞掉,避免键漏到下方 composer)。
bool onPickerChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (!g_picker_open || g_picker_tab != 0) return false;
    if (!g_picker_search_focus) return false;
    // 交给 InputBox:printable 插入、0x08 退格、Ctrl+A/C/V/X。Enter/Tab 由 onPickerKey 处理。
    g_picker_search.onChar(c, ctrl, hwnd);
    // 过滤条件变了 → 选中回到首格,滚动回顶(paint 会按新结果重新钳)。
    g_picker_sel_idx = -1;
    g_emoji_scroll_y = 0.0f;
    return true;   // 吞掉:emoji tab 打字只喂搜索框,不漏给 composer
}

// picker 打开时的按键 → 方向键导航 / Enter 发送 / Tab 切 tab。返回 true=已消费。
// 导航基于当前"过滤后"的结果集(g_picker_search 决定),sel_idx 是结果集内的下标。
bool onPickerKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!g_picker_open) return false;
    const int cols = 8;
    if (vk == VK_TAB) {
        setPickerTabSmooth(g_picker_tab == 0 ? 1 : 0);
        g_picker_sel_idx = -1;
        return true;
    }
    if (g_picker_tab != 0) return false;   // 导航目前只覆盖 emoji tab
    std::vector<int> filtered = filteredEmojiIndices(g_picker_search.text);
    int total = (int)filtered.size();
    if (vk == VK_DOWN || vk == VK_UP || vk == VK_LEFT || vk == VK_RIGHT) {
        if (total == 0) return true;
        if (g_picker_sel_idx < 0) { g_picker_sel_idx = 0; return true; }
        int idx = g_picker_sel_idx;
        if (vk == VK_RIGHT) idx = (std::min)(idx + 1, total - 1);
        else if (vk == VK_LEFT) idx = (std::max)(idx - 1, 0);
        else if (vk == VK_DOWN) idx = (std::min)(idx + cols, total - 1);
        else if (vk == VK_UP)   idx = (std::max)(idx - cols, 0);
        g_picker_sel_idx = idx;
        // 滚动跟随:让选中行可见
        float cell = 37.0f;
        float sel_top = (float)(idx / cols) * cell;
        if (sel_top < g_emoji_scroll_y) g_emoji_scroll_y = sel_top;
        else if (sel_top + cell > g_emoji_scroll_y + g_emoji_grid_h_last)
            g_emoji_scroll_y = sel_top + cell - g_emoji_grid_h_last;
        return true;
    }
    if (vk == VK_RETURN) {
        if (total == 0) return true;
        int sel = g_picker_sel_idx >= 0 ? g_picker_sel_idx : 0;
        if (sel >= 0 && sel < total) sendEmojiGlyph(kEmoji[filtered[sel]]);
        return true;
    }
    // 搜索框聚焦时吞掉 Del/Home/End,避免漏到下方 composer 误编辑其文本。
    if (g_picker_search_focus && (vk == VK_DELETE || vk == VK_HOME || vk == VK_END)) {
        g_picker_search.onKey(vk, shift, ctrl);
        g_picker_sel_idx = -1;
        return true;
    }
    (void)hwnd; (void)shift; (void)ctrl;
    return false;
}

// 发送输入框:先逐个发暂存附件(各自 image/gif/video 消息),再发文本(若有),清空。
void sendComposer(HWND hwnd) {
    if (!canWriteActiveChannel()) return;
    bool sent_any = false;
    for (auto& att : g_composer_attachments) {
        appendMedia(att.path);   // 走现有 optimistic + 上传管线,各自成一条图片消息
        sent_any = true;
    }
    g_composer_attachments.clear();
    if (!g_composer.text.empty()) {
        if (sendTextMessage(hwnd, g_composer.text)) { g_composer.reset(); sent_any = true; }
    } else if (sent_any) {
        g_composer.reset();
    }
    (void)sent_any;
}

void onChar(HWND hwnd, wchar_t c, bool ctrl) {
    if (!g_focus_composer) return;
    if (!canWriteActiveChannel()) {
        g_focus_composer = false;
        return;
    }
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    // Backspace 且光标在文本最前 + 有附件 → 删最后一个附件(chip 作为整体删)
    if (c == 0x08 && !ctrl && g_composer.cursor == 0 && !g_composer.hasSelection()
        && !g_composer_attachments.empty()) {
        g_composer_attachments.pop_back();
        return;
    }
    int r = g_composer.onChar(c, ctrl, shift, hwnd);
    if (r == 2) {   // Enter(无 Shift)-> 发送(文本 + 暂存附件一起)
        if (!g_composer.text.empty() || !g_composer_attachments.empty()) {
            sendComposer(hwnd);
        }
    }
}

void onKey(HWND hwnd, int vk, bool shift, bool ctrl) {
    if (!g_focus_composer) return;
    if (!canWriteActiveChannel()) {
        g_focus_composer = false;
        return;
    }
    // Ctrl+Shift+Z -> 重做(WM_CHAR 收不到,这里处理)
    if (ctrl && shift && (vk == 'Z')) { g_composer.redo(); return; }
    // Enter 的发送 / Shift+Enter 换行统一在 onChar(WM_CHAR)处理,避免 KEYDOWN+CHAR 双触发。
    if (vk == VK_RETURN) return;
    if (vk == VK_ESCAPE) { g_focus_composer = false; return; }
    g_composer.onKey(vk, shift, ctrl);
}

}  // namespace launcher::d2d::chat
