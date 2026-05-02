#pragma once

// 注册表"隐写"：把敏感字段藏在系统看起来"很官方"的子键下。
// 真实路径只在 .cpp 里以 CRYPT_STR 形式出现，外部接口只看 Slot 枚举。
//
// 写入流程：value -> DPAPI seal -> AES-GCM(key=HWID 派生) -> RegSetValueExW(REG_BINARY)
//
// Why: 用户要求凭据"全部都在注册表 我们放的隐蔽一点"。用 IE Zone 子键 + 随机 GUID 名
//      混在一堆系统值里，单纯 grep 不会暴露。

#include "app/common.h"
#include <vector>

namespace launcher::storage {

enum class Slot : u8 {
    UsernameHash    = 0,
    PasswordToken   = 1,
    SessionToken    = 2,
    HwidBinding     = 3,
    LastLoginUtc    = 4,
    SubscriptionTier= 5,
    SubscriptionExp = 6,
};

class Registry {
public:
    static Registry& instance();

    Status writeBytes(Slot slot, const std::vector<u8>& bytes);
    Result<std::vector<u8>> readBytes(Slot slot);
    Status erase(Slot slot);

    // 字符串便捷封装（内部调用 writeBytes/readBytes）
    Status writeString(Slot slot, const std::string& s);
    Result<std::string> readString(Slot slot);

private:
    Registry() = default;
    LAUNCHER_DISALLOW_COPY(Registry);
};

}  // namespace launcher::storage
