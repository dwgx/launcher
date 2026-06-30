// 统一的错误处理 helper。
//
// 设计目标：
//   * 返回给客户端的 body 只用通用文案，不泄露内部错误细节（DB schema、SQL、路径等）。
//   * 真实错误 + 调用位置走 tracing::error! 记进服务端日志，便于排查。
//
// 用法：
//   * 返回 (StatusCode, String) 的 handler：`.map_err(internal)?`
//   * admin SSR handler（只要 String 错误）：`.map_err(internal_msg)?`

use axum::http::StatusCode;
use std::panic::Location;

const GENERIC_500: &str = "internal server error";

/// 记录内部错误（含调用位置）并返回不泄露细节的 500 响应。
#[track_caller]
pub fn internal<E: std::fmt::Display>(e: E) -> (StatusCode, String) {
    let loc = Location::caller();
    tracing::error!(
        target: "launcher::internal_error",
        location = %format!("{}:{}", loc.file(), loc.line()),
        error = %e,
        "internal server error",
    );
    (StatusCode::INTERNAL_SERVER_ERROR, GENERIC_500.to_string())
}

/// 同 [`internal`]，但只返回通用文案字符串（给 admin SSR handler 用）。
#[track_caller]
pub fn internal_msg<E: std::fmt::Display>(e: E) -> String {
    let loc = Location::caller();
    tracing::error!(
        target: "launcher::internal_error",
        location = %format!("{}:{}", loc.file(), loc.line()),
        error = %e,
        "internal server error",
    );
    GENERIC_500.to_string()
}
