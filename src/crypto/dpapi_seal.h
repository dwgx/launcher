#pragma once

// DPAPI 封装：CryptProtectData 绑定当前用户，返回密文。
// Why: 注册表被 dump 走也无法在另一台机器解密；user-bound 比 machine-bound 更严

#include "app/common.h"
#include <vector>

namespace launcher::crypto {

// entropy 可空；建议传 HWID 派生值进一步绑机器
Result<std::vector<u8>> dpapiSeal(const std::vector<u8>& plain,
                                  const std::vector<u8>& entropy = {});

Result<std::vector<u8>> dpapiUnseal(const std::vector<u8>& cipher,
                                    const std::vector<u8>& entropy = {});

}  // namespace launcher::crypto
