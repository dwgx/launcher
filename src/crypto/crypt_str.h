#pragma once

// 编译期 XOR 字符串混淆。用法：CRYPT_STR("CreateProcessW")
//
// Why: 加壳前需要把"显眼"的 API 名、注册表路径、URL 这些字符串干掉。
//      运行时按需解密成 std::string；解密 key 派生自下标，没有静态全字符表。
//
// 限制：长度上限 256（够用），不要塞二进制。结果是窄字符串。

#include "app/common.h"
#include <array>
#include <cstring>

namespace launcher::crypto {

constexpr u8 kCryptStrSeed = 0x5A;

constexpr u8 derive_key(size_t i, size_t n) {
    // Why: 引入长度 n 让相同前缀的不同长度字符串密文不一致，反字符串聚类
    return static_cast<u8>(kCryptStrSeed
                           ^ ((i * 0x9E) & 0xFF)
                           ^ ((n * 0x37) & 0xFF));
}

template <size_t N>
struct CipherText {
    std::array<u8, N> bytes{};
    constexpr CipherText(const char (&src)[N]) {
        for (size_t i = 0; i < N; ++i) {
            bytes[i] = static_cast<u8>(src[i]) ^ derive_key(i, N);
        }
    }
};

// 解密到栈缓冲并返回 std::string，避免堆分配前的明文残留
template <size_t N>
inline std::string decrypt(const CipherText<N>& c) {
    char buf[N];
    for (size_t i = 0; i < N; ++i) {
        buf[i] = static_cast<char>(c.bytes[i] ^ derive_key(i, N));
    }
    std::string s(buf, N - 1);  // 去掉编译期带的 \0
    std::memset(buf, 0, N);     // 擦栈，减少明文滞留
    return s;
}

}  // namespace launcher::crypto

// Why: 用 lambda + constexpr 局部对象保证密文进 .rdata，明文绝不静态出现
#define CRYPT_STR(literal) ([]() -> ::std::string {                       \
    static constexpr ::launcher::crypto::CipherText<sizeof(literal)> _c{literal}; \
    return ::launcher::crypto::decrypt(_c);                                 \
}())

// 宽字符版本：解密后再 MultiByteToWideChar
#define CRYPT_WSTR(literal) ([]() -> ::std::wstring {                     \
    auto narrow = CRYPT_STR(literal);                                      \
    return ::std::wstring(narrow.begin(), narrow.end());                   \
}())
