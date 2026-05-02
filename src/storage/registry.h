#pragma once

// 注册表"隐写"层：把 8 个敏感 slot 拆到 8 个看似系统遗留的路径里。
//
// 每个 slot 都用：
//   1) 不同的 HKCU 子键路径（Windows 自带或常见软件留下的真实路径）
//   2) GUID/UUID 风格的值名，混在该路径已有的值里
//   3) DPAPI(user) 封装 + AES-GCM 二次加密（密钥 = HWID 派生）
//   4) 第一次写入时随机生成 3-5 个 fake siblings（同前缀格式的假数据）
//
// 路径选择遵循：
//   - 父路径必须本来就存在（系统/常用软件留下的）
//   - 父路径平时被读但不常写，不会触发用户/反作弊关注
//   - 不要选 Run/RunOnce/Startup（明显敏感）

#include "app/common.h"
#include <vector>

namespace launcher::storage {

enum class Slot : u8 {
    UsernameHash     = 0,
    PasswordToken    = 1,
    SessionToken     = 2,
    HwidBinding      = 3,
    LastLoginUtc     = 4,
    SubscriptionTier = 5,
    SubscriptionExp  = 6,
    DeviceSeed       = 7,    // 随机 32B，用作 AES-GCM 派生 key 的 salt
};

class Registry {
public:
    static Registry& instance();

    Status writeBytes(Slot slot, const std::vector<u8>& bytes);
    Result<std::vector<u8>> readBytes(Slot slot);
    Status erase(Slot slot);

    Status writeString(Slot slot, const std::string& s);
    Result<std::string> readString(Slot slot);

    // 强清理：擦掉所有 slot + fake siblings。卸载/退出登录调用
    void   wipeAll();

    // 第一次启动时往每个 slot 父键灌假数据，反扫描
    void   sprinkleDecoys();

private:
    Registry() = default;
    LAUNCHER_DISALLOW_COPY(Registry);
};

}  // namespace launcher::storage
