use axum::http::StatusCode;

#[derive(Clone, Copy)]
pub(crate) struct MediaKind {
    pub mime: &'static str,
    pub ext: &'static str,
    pub category: &'static str,
}

pub(crate) fn detect_media(bytes: &[u8]) -> Option<MediaKind> {
    if bytes.len() >= 8 && bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
        return Some(MediaKind { mime: "image/png", ext: "png", category: "image" });
    }
    if bytes.len() >= 3 && bytes.starts_with(b"\xff\xd8\xff") {
        return Some(MediaKind { mime: "image/jpeg", ext: "jpg", category: "image" });
    }
    if bytes.len() >= 6 && (bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a")) {
        return Some(MediaKind { mime: "image/gif", ext: "gif", category: "image" });
    }
    if bytes.len() >= 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        return Some(MediaKind { mime: "image/webp", ext: "webp", category: "image" });
    }
    if bytes.len() >= 12 && &bytes[4..8] == b"ftyp" {
        return Some(MediaKind { mime: "video/mp4", ext: "mp4", category: "video" });
    }
    if bytes.len() >= 4 && bytes.starts_with(&[0x1a, 0x45, 0xdf, 0xa3]) {
        return Some(MediaKind { mime: "video/webm", ext: "webm", category: "video" });
    }
    None
}

pub(crate) fn validate_avatar(bytes: &[u8]) -> Result<MediaKind, (StatusCode, String)> {
    let kind = detect_media(bytes).ok_or((
        StatusCode::UNSUPPORTED_MEDIA_TYPE,
        "png/jpeg/gif avatar required".into(),
    ))?;
    if !matches!(kind.mime, "image/png" | "image/jpeg" | "image/gif") {
        return Err((StatusCode::UNSUPPORTED_MEDIA_TYPE, "png/jpeg/gif avatar required".into()));
    }
    Ok(kind)
}

pub(crate) fn validate_upload(bytes: &[u8]) -> Result<MediaKind, (StatusCode, String)> {
    detect_media(bytes).ok_or((
        StatusCode::UNSUPPORTED_MEDIA_TYPE,
        "supported media required".into(),
    ))
}

pub(crate) fn is_hex_sha256(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}
