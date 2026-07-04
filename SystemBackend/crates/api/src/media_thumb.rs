// 缩略图 / 占位符生成（Wave 3）。
//
// 上传是冷路径、读取是热路径 —— 在上传时一次性生成多档缩略图 + BlurHash 占位串，
// 之后 GET /api/media 与 /api/avatar 直接按需返回对应档位，客户端用 BlurHash 先画
// 模糊预览再淡入真图。
//
// 设计要点：
//   * 解码 / 缩放 / 重编码全是 CPU 密集，整块放进 tokio::task::spawn_blocking，
//     避免占用 async worker（对齐 profile.rs 把阻塞 fs 换成 tokio::fs 的初衷）。
//   * 解压炸弹防护：解码前用 image::Limits 限制最大宽高 + 分配上限；解码后再兜底
//     校验像素总数。任一超限或解码失败 —— 不 panic，返回 None，调用方只存原图
//     (has_thumbs=false)。
//   * 缩放只向下（downscale-only）：档位 >= 原图最长边则跳过，绝不放大。
//   * 照片编码 JPEG，带 alpha 的图编码 PNG（WIC 恒能解 JPEG/PNG，避免客户端 codec 依赖）。
//   * 兄弟文件命名可从 sha 派生：<stem>_s<slot>.<ext>，与原图同目录。

use image::{imageops::FilterType, DynamicImage, ImageReader};
use std::io::Cursor;
use std::path::{Path, PathBuf};

/// 单张图允许的最大边（解码前限制），超过视为解压炸弹拒绝生成缩略图。
const MAX_EDGE: u32 = 12_000;
/// 解码分配上限（字节），~256MB，防止巨图 OOM。
const MAX_ALLOC_BYTES: u64 = 256 * 1024 * 1024;
/// 解码后兜底：像素总数上限（宽*高），~64MP。
const MAX_PIXELS: u64 = 64 * 1_000_000;

/// BlurHash 分量（横 x 纵）。4x3 是照片/宽幅图的常见取值，串长可控。
const BLURHASH_X: u32 = 4;
const BLURHASH_Y: u32 = 3;
/// 计算 BlurHash 前先把图缩到这个最长边，避免对大图逐像素编码。
const BLURHASH_SRC_EDGE: u32 = 64;

/// 媒体上传使用的档位（最长边像素）。与变体服务白名单一致：64/128/256/400/512。
pub const MEDIA_SLOTS: &[u32] = &[64, 128, 256, 400, 512];
/// 头像档位。
pub const AVATAR_SLOTS: &[u32] = &[64, 128];
/// 变体服务允许的档位白名单（拒绝任意尺寸，防 resize-DoS）。
pub const ALLOWED_SLOTS: &[u32] = &[64, 128, 256, 400, 512];

/// 生成结果。失败（解码/编码异常、非静态图）时返回 None，调用方只存原图。
pub struct ThumbResult {
    pub width: u32,
    pub height: u32,
    pub blurhash: String,
    /// 实际落盘的档位（升序），用于变体解析时判断“最近存在档”。
    pub variants: Vec<u32>,
}

/// 兄弟缩略图文件名：`<stem>_s<slot>.<ext>`。
pub fn variant_filename(stem: &str, slot: u32, ext: &str) -> String {
    format!("{}_s{}.{}", stem, slot, ext)
}

/// 从档位与是否 alpha 推导缩略图扩展名（jpg / png）。
pub fn variant_ext(has_alpha: bool) -> &'static str {
    if has_alpha {
        "png"
    } else {
        "jpg"
    }
}

/// 把请求的尺寸对齐到白名单里 >= 请求值的最小档；超出最大档返回 None（用原图）。
pub fn resolve_slot(requested: u32) -> Option<u32> {
    ALLOWED_SLOTS.iter().copied().find(|&s| s >= requested)
}

/// 异步入口：在 spawn_blocking 内解码 + 生成所有档缩略图 + BlurHash，兄弟文件写到 `dir`。
///
/// - `bytes`：原始文件字节。
/// - `dir`：缩略图落盘目录（与原图同目录）。
/// - `stem`：文件名主干（media 用 sha，头像用 uid 字符串）。
/// - `slots`：目标档位集合（MEDIA_SLOTS / AVATAR_SLOTS）。
///
/// 返回 None 表示无法安全生成缩略图（解压炸弹 / 解码失败 / 非位图），调用方只存原图。
pub async fn generate(
    bytes: bytes::Bytes,
    dir: PathBuf,
    stem: String,
    slots: &'static [u32],
) -> Option<ThumbResult> {
    let res = tokio::task::spawn_blocking(move || generate_blocking(&bytes, &dir, &stem, slots))
        .await
        .ok()
        .flatten();
    res
}

fn decode_guarded(bytes: &[u8]) -> Option<DynamicImage> {
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(MAX_EDGE);
    limits.max_image_height = Some(MAX_EDGE);
    limits.max_alloc = Some(MAX_ALLOC_BYTES);

    let reader = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .ok()?;
    let mut reader = reader;
    reader.limits(limits);
    let img = reader.decode().ok()?;

    let (w, h) = (img.width() as u64, img.height() as u64);
    if w == 0 || h == 0 || w.saturating_mul(h) > MAX_PIXELS {
        return None;
    }
    Some(img)
}

fn generate_blocking(
    bytes: &[u8],
    dir: &Path,
    stem: &str,
    slots: &[u32],
) -> Option<ThumbResult> {
    let img = decode_guarded(bytes)?;
    let w = img.width();
    let h = img.height();
    let longest = w.max(h);
    let has_alpha = img.color().has_alpha();
    let ext = variant_ext(has_alpha);

    // BlurHash：从小图编码，失败不致命（返回空串，占位符降级为纯色/无）。
    let blurhash = compute_blurhash(&img).unwrap_or_default();

    // 目录可能已由原图写入创建，这里兜底再建一次。
    if std::fs::create_dir_all(dir).is_err() {
        // 目录建不出，无法落盘缩略图，但 blurhash/w/h 仍可用 —— 返回空 variants。
        return Some(ThumbResult {
            width: w,
            height: h,
            blurhash,
            variants: Vec::new(),
        });
    }

    let mut variants = Vec::new();
    for &slot in slots {
        // downscale-only：档位 >= 原图最长边则跳过（不放大）。
        if slot >= longest {
            continue;
        }
        let resized = img.resize(slot, slot, FilterType::Lanczos3);
        let out_path = dir.join(variant_filename(stem, slot, ext));
        if encode_variant(&resized, has_alpha, &out_path).is_ok() {
            variants.push(slot);
        }
    }

    Some(ThumbResult {
        width: w,
        height: h,
        blurhash,
        variants,
    })
}

fn encode_variant(img: &DynamicImage, has_alpha: bool, path: &Path) -> Result<(), image::ImageError> {
    if has_alpha {
        // 保留 alpha：PNG。
        img.to_rgba8().save_with_format(path, image::ImageFormat::Png)
    } else {
        // 照片：JPEG q≈82。用 encoder 显式控质量。
        let rgb = img.to_rgb8();
        let mut buf = Vec::new();
        {
            let mut enc = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut buf, 82);
            enc.encode(rgb.as_raw(), rgb.width(), rgb.height(), image::ExtendedColorType::Rgb8)?;
        }
        std::fs::write(path, &buf).map_err(image::ImageError::IoError)
    }
}

fn compute_blurhash(img: &DynamicImage) -> Option<String> {
    // 缩到小图再编码，控制耗时。
    let small = img.resize(BLURHASH_SRC_EDGE, BLURHASH_SRC_EDGE, FilterType::Triangle);
    let rgba = small.to_rgba8();
    blurhash::encode(
        BLURHASH_X,
        BLURHASH_Y,
        rgba.width(),
        rgba.height(),
        rgba.as_raw(),
    )
    .ok()
}
