#pragma once

// 严格硬件指纹：用于检测账号共享 + 多机滥用。
//
// 设计目标：**任意一项硬件改变都让 fingerprint() 变化**，
//   - 重装系统 (MachineGuid 变) → fail
//   - 换主板 (BaseBoard SerialNumber 变) → fail
//   - 换 CPU (CPU brand/sig 变) → fail
//   - 换显卡 (GPU PNPDeviceID 变) → fail
//   - 换网卡 (主 MAC 变) → fail
//   - 换硬盘 (System disk SN/UUID 变) → fail
//   - TPM 重置 (Endorsement Key 变) → fail
//
// 用户合法换硬件时走"重新绑定请求"：客户端把 PartsDiff 发到
// /api/hwid/rebind/request，管理员在后台审核同意；同意后下发新 token，
// 客户端覆盖注册表里的旧 binding。
//
// 输出：
//   * fingerprint_hex —— SHA-256(序列化 parts) 的 hex；上报给后端做严格比对
//   * parts (CollectedParts) —— 每一项原始值；用于 PartsDiff 展示
//
// 原则：宁可 false-negative 多一些，也不要 false-positive 让攻击者通过

#include "app/common.h"
#include <vector>
#include <unordered_map>

namespace launcher::native {

enum class HwidPart : u8 {
    BaseBoardSerial   = 0,
    BiosUuid          = 1,
    BiosVersion       = 2,
    CpuSignature      = 3,
    SystemDiskSerial  = 4,
    VolumeSerial      = 5,
    PrimaryMacPhys    = 6,
    AllMacsHash       = 7,    // 所有物理网卡 MAC 排序后哈希
    GpuPnpId          = 8,
    GpuVendor         = 9,
    MachineGuid       = 10,
    TpmEkPub          = 11,   // TPM 2.0 Endorsement Key public，无 TPM 留空
    SmbiosSystemUuid  = 12,
    DisplayEdidHash   = 13,   // 主显示器 EDID 哈希（换显示器也变）
    PartCount_        = 14,
};

struct HwidParts {
    std::unordered_map<HwidPart, std::string> values;

    bool has(HwidPart p) const {
        auto it = values.find(p); return it != values.end() && !it->second.empty();
    }
    const std::string& get(HwidPart p) const {
        static std::string empty;
        auto it = values.find(p); return it == values.end() ? empty : it->second;
    }
    int filledCount() const {
        int n = 0; for (auto& kv : values) if (!kv.second.empty()) ++n; return n;
    }
};

// 严格度阈值：低于这个值的指纹视为不可信，登录直接拒
//
// 把 SystemDiskSerial / MachineGuid / BaseBoardSerial / BiosUuid 标为 *core*，
// 任意一个 core 缺失即拒绝；其余作为辅助。
constexpr int kMinPartsForTrust = 8;
constexpr int kCorePartsRequired = 4;

class HwidCollector {
public:
    Result<HwidParts>   collectParts();
    Result<std::string> collectFingerprint();   // SHA-256 hex

    // 计算两个 parts 的差异（用于重绑定审核 UI）
    static std::vector<HwidPart> diff(const HwidParts& a, const HwidParts& b);
    static const char* partName(HwidPart p);

    static bool isCore(HwidPart p) {
        return p == HwidPart::SystemDiskSerial
            || p == HwidPart::MachineGuid
            || p == HwidPart::BaseBoardSerial
            || p == HwidPart::BiosUuid;
    }
};

// 后端 /api/hwid/rebind/request 的 JSON payload 结构
struct RebindRequest {
    std::string old_fingerprint_hex;
    std::string new_fingerprint_hex;
    HwidParts   new_parts;
    std::string reason_user_typed;     // 用户填的理由（"换了主板"等）
};

}  // namespace launcher::native
