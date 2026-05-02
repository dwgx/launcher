#include "crypto/crypt_str.h"

// 头文件已是模板/inline，本 TU 仅提供占位以便 CMake 能链接，
// 后续若要加 IsDebuggerPresent 联动密钥派生再扩展
namespace launcher::crypto {
namespace {
volatile u8 kAnchor = 0;  // 防止链接器把整个 TU 优化掉
}
void crypt_str_keepalive() { kAnchor = static_cast<u8>(kAnchor + 1); }
}  // namespace launcher::crypto
