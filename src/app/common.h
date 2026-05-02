#pragma once

// Why: 全项目共享的最小依赖头，避免各文件零散 #include 顺序错误。
// 不要在这里放重型 STL 或 Windows.h，那些在 .cpp 内按需引入。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <utility>

namespace launcher {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// Why: 禁用异常 (/EHs-c-) + 禁用 RTTI (/GR-) 后用结构化错误码代替 throw
template <typename T>
struct Result {
    T value{};
    int error_code{0};
    bool ok() const { return error_code == 0; }
    explicit operator bool() const { return ok(); }
};

struct Status {
    int code{0};
    bool ok() const { return code == 0; }
    static Status Ok() { return {0}; }
    static Status Err(int c) { return {c}; }
};

#define LAUNCHER_DISALLOW_COPY(T)               \
    T(const T&) = delete;                       \
    T& operator=(const T&) = delete

#define LAUNCHER_DISALLOW_MOVE(T)               \
    T(T&&) = delete;                            \
    T& operator=(T&&) = delete

}  // namespace launcher
