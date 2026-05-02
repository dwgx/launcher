#include "ui/render/font_manager.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkString.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_directory.h"
#include "modules/skshaper/include/SkShaper.h"

#include <spdlog/spdlog.h>
#include <vector>

namespace launcher::ui::render {

struct FontManager::Impl {
    sk_sp<SkFontMgr>   mgr;
    sk_sp<SkTypeface>  primary;     // 英数 Space Grotesk
    sk_sp<SkTypeface>  jp;          // BIZ UDPGothic
    sk_sp<SkTypeface>  cn;          // Source Han Sans CN
    sk_sp<SkTypeface>  mono;        // DejaVu Mono
};

FontManager::FontManager()  : m_impl(new Impl()) {}
FontManager::~FontManager() { delete m_impl; }

namespace {
sk_sp<SkTypeface> match_family(const sk_sp<SkFontMgr>& mgr, const char* family) {
    if (!mgr) return nullptr;
    return mgr->matchFamilyStyle(family, SkFontStyle::Normal());
}
}  // namespace

Status FontManager::loadFromDirectory(const std::string& dir) {
    m_impl->mgr = SkFontMgr_New_Custom_Directory(dir.c_str());
    if (!m_impl->mgr) {
        spdlog::error("SkFontMgr_New_Custom_Directory failed for {}", dir);
        return Status::Err(1);
    }
    m_impl->primary = match_family(m_impl->mgr, "Space Grotesk");
    m_impl->jp      = match_family(m_impl->mgr, "BIZ UDPGothic");
    m_impl->cn      = match_family(m_impl->mgr, "Source Han Sans CN");
    m_impl->mono    = match_family(m_impl->mgr, "DejaVu Sans Mono");

    if (!m_impl->primary && !m_impl->cn) {
        spdlog::warn("No primary or CN typeface found in {}", dir);
        return Status::Err(2);
    }
    return Status::Ok();
}

// Why: 简化实现 — Phase 1 只画 ASCII，所以走 SkFont；后续中日文再接 SkShaper
void FontManager::drawShapedText(SkCanvas* canvas, const std::string& utf8,
                                 f32 x, f32 y, f32 size_px, u32 sk_color) {
    if (!canvas) return;
    sk_sp<SkTypeface> tf = m_impl->primary
        ? m_impl->primary
        : (m_impl->cn ? m_impl->cn : nullptr);
    if (!tf) return;

    SkFont font(tf, size_px);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    font.setSubpixel(true);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(sk_color);

    canvas->drawSimpleText(utf8.data(), utf8.size(),
                           SkTextEncoding::kUTF8, x, y, font, paint);
}

f32 FontManager::measureWidth(const std::string& utf8, f32 size_px) {
    sk_sp<SkTypeface> tf = m_impl->primary ? m_impl->primary : m_impl->cn;
    if (!tf) return 0.0f;
    SkFont font(tf, size_px);
    return font.measureText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8);
}

}  // namespace launcher::ui::render
