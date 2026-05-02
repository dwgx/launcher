#pragma once

// 硬件指纹：用于检测账号共享/多机。
// 多源拼接（每一项都可能丢，但全部丢的概率极低）：
//   1. SMBIOS BaseBoard SerialNumber  (WMI Win32_BaseBoard)
//   2. SMBIOS Bios UUID               (WMI Win32_ComputerSystemProduct.UUID)
//   3. CPU 序列(__cpuid 0/1)          (无 SN 的 CPU 走 Brand+Family+Model 派生)
//   4. 系统盘 VolumeSerial            (GetVolumeInformationW C:\)
//   5. 主网卡 MAC（带物理标志）        (GetAdaptersAddresses)
//   6. 显卡 PNPDeviceID                (WMI Win32_VideoController)
//   7. Windows MachineGuid             (HKLM\SOFTWARE\Microsoft\Cryptography)
//
// 输出：64 字节 SHA-256 hex；调用方再做盐化/版本号附加后上报后端

#include "app/common.h"

namespace launcher::native {

struct HwidParts {
    std::string baseboard_serial;
    std::string bios_uuid;
    std::string cpu_sig;
    std::string volume_serial;
    std::string primary_mac;
    std::string gpu_pnp;
    std::string machine_guid;

    bool isUsable() const;   // 有效项 >= 4
};

class HwidCollector {
public:
    Result<HwidParts> collectParts();
    Result<std::string> collectFingerprint();   // 返回 SHA-256 hex
};

}  // namespace launcher::native
