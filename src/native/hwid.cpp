#include "native/hwid.h"
#include "crypto/crypt_str.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WbemIdl.h>
#include <comdef.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <sodium.h>
#include <tbs.h>

#include <spdlog/spdlog.h>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "tbs.lib")

namespace launcher::native {

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace((u8)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && std::isspace((u8)s[i])) ++i;
    return s.substr(i);
}
std::string toHex(const u8* d, size_t n) {
    static const char H[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2*i] = H[(d[i] >> 4) & 0xF]; s[2*i+1] = H[d[i] & 0xF]; }
    return s;
}

class WmiSession {
public:
    static WmiSession& instance() { static WmiSession s; return s; }
    IWbemServices* svc() { return m_svc; }
    bool ok() const { return m_ok; }
    ~WmiSession() {
        if (m_svc) m_svc->Release();
        if (m_loc) m_loc->Release();
        if (m_co_init) ::CoUninitialize();
    }
private:
    WmiSession() {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_co_init = (hr == S_OK || hr == S_FALSE);
        ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        if (FAILED(::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                    IID_IWbemLocator, (void**)&m_loc))) return;
        BSTR ns = ::SysAllocString(L"ROOT\\CIMV2");
        hr = m_loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_svc);
        ::SysFreeString(ns);
        if (FAILED(hr)) { m_svc = nullptr; return; }
        ::CoSetProxyBlanket(m_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        m_ok = true;
    }
    bool m_co_init{false}, m_ok{false};
    IWbemLocator*  m_loc{nullptr};
    IWbemServices* m_svc{nullptr};
};

std::vector<std::string> wmiQueryAll(const wchar_t* wql, const wchar_t* prop) {
    std::vector<std::string> out;
    auto& s = WmiSession::instance();
    if (!s.ok()) return out;
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = ::SysAllocString(L"WQL"), q = ::SysAllocString(wql);
    HRESULT hr = s.svc()->ExecQuery(lang, q,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    ::SysFreeString(lang); ::SysFreeString(q);
    if (FAILED(hr) || !en) return out;
    IWbemClassObject* obj = nullptr; ULONG ret = 0;
    while (en->Next(WBEM_INFINITE, 1, &obj, &ret) == WBEM_S_NO_ERROR && ret) {
        VARIANT v; ::VariantInit(&v);
        if (obj->Get(prop, 0, &v, nullptr, nullptr) == S_OK && v.vt == VT_BSTR && v.bstrVal) {
            out.push_back(trim(toUtf8(v.bstrVal)));
        }
        ::VariantClear(&v); obj->Release(); obj = nullptr;
    }
    en->Release();
    return out;
}

std::string wmiOne(const wchar_t* wql, const wchar_t* prop) {
    auto v = wmiQueryAll(wql, prop);
    return v.empty() ? std::string{} : v.front();
}

std::string queryMachineGuid() {
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Cryptography",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return {};
    wchar_t b[128] = {0}; DWORD cb = sizeof(b);
    LSTATUS r = ::RegQueryValueExW(hk, L"MachineGuid", nullptr, nullptr, (LPBYTE)b, &cb);
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS ? toUtf8(std::wstring(b)) : std::string{};
}

std::string queryVolumeSerial() {
    DWORD sn = 0;
    if (!::GetVolumeInformationW(L"C:\\", nullptr, 0, &sn, nullptr, nullptr, nullptr, 0)) return {};
    char b[16]; ::sprintf_s(b, "%08X", sn); return std::string(b);
}

std::string queryCpuSig() {
    int regs[4] = {0}; char brand[0x40] = {0};
    ::__cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004) {
        ::__cpuid((int*)brand,      0x80000002);
        ::__cpuid((int*)(brand+16), 0x80000003);
        ::__cpuid((int*)(brand+32), 0x80000004);
    }
    ::__cpuid(regs, 1);
    char b[128];
    ::sprintf_s(b, "%s|%08X-%08X-%08X-%08X",
        trim(brand).c_str(), (u32)regs[0], (u32)regs[1], (u32)regs[2], (u32)regs[3]);
    return std::string(b);
}

// 物理 MAC（排除 loopback / 虚拟）
std::vector<std::string> queryAllPhysicalMacs() {
    ULONG cb = 0;
    ::GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, nullptr, &cb);
    if (cb == 0) return {};
    std::vector<u8> buf(cb);
    auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (::GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, head, &cb) != NO_ERROR) return {};
    std::vector<std::string> macs;
    for (auto* a = head; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->PhysicalAddressLength != 6) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        // 排除虚拟（VirtualBox / Hyper-V）
        std::wstring desc(a->Description ? a->Description : L"");
        if (desc.find(L"Virtual") != std::wstring::npos) continue;
        if (desc.find(L"VPN") != std::wstring::npos) continue;
        char m[32];
        ::sprintf_s(m, "%02X-%02X-%02X-%02X-%02X-%02X",
            a->PhysicalAddress[0], a->PhysicalAddress[1], a->PhysicalAddress[2],
            a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
        macs.emplace_back(m);
    }
    std::sort(macs.begin(), macs.end());
    return macs;
}

std::string sha256Hex(const std::string& blob) {
    if (sodium_init() < 0) return {};
    u8 h[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(h, (const u8*)blob.data(), blob.size());
    return toHex(h, sizeof(h));
}

// TPM 2.0 Endorsement Key public — 任何 TPM 重置 (clear) 后此值变
std::string queryTpmEkPub() {
    // 简化实现：只调用 Tbsi_Get_TCG_Log 看 TPM 是否在；EK 取出要走 TPM 命令较繁琐，
    // 占位为 device version；后续可扩展为真 EK 取出
    TBS_CONTEXT_PARAMS2 params{};
    params.version = TBS_CONTEXT_VERSION_TWO;
    params.includeTpm20 = 1;
    TBS_HCONTEXT ctx = nullptr;
    if (Tbsi_Context_Create(reinterpret_cast<PCTBS_CONTEXT_PARAMS>(&params), &ctx) != TBS_SUCCESS) return {};
    TPM_DEVICE_INFO di{};
    if (Tbsi_GetDeviceInfo(sizeof(di), &di) != TBS_SUCCESS) {
        Tbsi_Context_Close(ctx); return {};
    }
    Tbsi_Context_Close(ctx);
    char b[64];
    ::sprintf_s(b, "tpm-ver=%lu-mfr=%lu-tpm-rev=%lu",
        (unsigned long)di.tpmVersion,
        (unsigned long)di.manufacturerVendorID,
        (unsigned long)di.tpmInterfaceType);
    return std::string(b);
}

// 主显示器 EDID — Windows 把 EDID 存在 SetupAPI 设备树
std::string queryPrimaryEdidHash() {
    HDEVINFO h = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return {};
    SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
    std::string hashInput;
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(h, i, &dd); ++i) {
        HKEY k = ::SetupDiOpenDevRegKey(h, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        BYTE edid[256] = {0}; DWORD cb = sizeof(edid), type = 0;
        if (::RegQueryValueExW(k, L"EDID", nullptr, &type, edid, &cb) == ERROR_SUCCESS) {
            hashInput.append(reinterpret_cast<const char*>(edid), cb);
        }
        ::RegCloseKey(k);
    }
    ::SetupDiDestroyDeviceInfoList(h);
    if (hashInput.empty()) return {};
    return sha256Hex(hashInput);
}

// 系统盘真实序列号（IOCTL_STORAGE_QUERY_PROPERTY）
std::string querySystemDiskSerial() {
    HANDLE h = ::CreateFileW(L"\\\\.\\PhysicalDrive0", 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;

    BYTE buf[1024] = {0};
    DWORD br = 0;
    BOOL ok = ::DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
        &q, sizeof(q), buf, sizeof(buf), &br, nullptr);
    ::CloseHandle(h);
    if (!ok) return {};

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    if (desc->SerialNumberOffset == 0) return {};
    const char* sn = reinterpret_cast<const char*>(buf) + desc->SerialNumberOffset;
    return trim(std::string(sn));
}

}  // namespace

const char* HwidCollector::partName(HwidPart p) {
    switch (p) {
        case HwidPart::BaseBoardSerial:  return "baseboard_serial";
        case HwidPart::BiosUuid:         return "bios_uuid";
        case HwidPart::BiosVersion:      return "bios_version";
        case HwidPart::CpuSignature:     return "cpu_signature";
        case HwidPart::SystemDiskSerial: return "system_disk_serial";
        case HwidPart::VolumeSerial:     return "c_volume_serial";
        case HwidPart::PrimaryMacPhys:   return "primary_mac";
        case HwidPart::AllMacsHash:      return "all_macs_hash";
        case HwidPart::GpuPnpId:         return "gpu_pnp_id";
        case HwidPart::GpuVendor:        return "gpu_vendor";
        case HwidPart::MachineGuid:      return "machine_guid";
        case HwidPart::TpmEkPub:         return "tpm_ek_pub";
        case HwidPart::SmbiosSystemUuid: return "smbios_system_uuid";
        case HwidPart::DisplayEdidHash:  return "display_edid_hash";
        default:                          return "?";
    }
}

Result<HwidParts> HwidCollector::collectParts() {
    HwidParts p;
    p.values[HwidPart::BaseBoardSerial]  = wmiOne(L"SELECT SerialNumber FROM Win32_BaseBoard", L"SerialNumber");
    p.values[HwidPart::BiosUuid]         = wmiOne(L"SELECT UUID FROM Win32_ComputerSystemProduct", L"UUID");
    p.values[HwidPart::BiosVersion]      = wmiOne(L"SELECT SMBIOSBIOSVersion FROM Win32_BIOS", L"SMBIOSBIOSVersion");
    p.values[HwidPart::SmbiosSystemUuid] = wmiOne(L"SELECT IdentifyingNumber FROM Win32_ComputerSystemProduct", L"IdentifyingNumber");
    p.values[HwidPart::GpuPnpId]         = wmiOne(L"SELECT PNPDeviceID FROM Win32_VideoController", L"PNPDeviceID");
    p.values[HwidPart::GpuVendor]        = wmiOne(L"SELECT AdapterCompatibility FROM Win32_VideoController", L"AdapterCompatibility");

    p.values[HwidPart::CpuSignature]     = queryCpuSig();
    p.values[HwidPart::VolumeSerial]     = queryVolumeSerial();
    p.values[HwidPart::SystemDiskSerial] = querySystemDiskSerial();
    p.values[HwidPart::MachineGuid]      = queryMachineGuid();
    p.values[HwidPart::TpmEkPub]         = queryTpmEkPub();
    p.values[HwidPart::DisplayEdidHash]  = queryPrimaryEdidHash();

    auto macs = queryAllPhysicalMacs();
    if (!macs.empty()) {
        p.values[HwidPart::PrimaryMacPhys] = macs.front();
        std::string concat;
        for (auto& m : macs) { concat += m; concat += '|'; }
        p.values[HwidPart::AllMacsHash] = sha256Hex(concat);
    }

    int filled = p.filledCount();
    int core_filled = 0;
    for (auto cp : { HwidPart::SystemDiskSerial, HwidPart::MachineGuid,
                     HwidPart::BaseBoardSerial, HwidPart::BiosUuid }) {
        if (p.has(cp)) ++core_filled;
    }

    if (filled < kMinPartsForTrust || core_filled < kCorePartsRequired) {
        spdlog::warn("HWID strict check failed: filled={} core_filled={}", filled, core_filled);
        return Result<HwidParts>{ p, 1 };
    }
    return Result<HwidParts>{ std::move(p), 0 };
}

Result<std::string> HwidCollector::collectFingerprint() {
    auto r = collectParts();
    if (!r.ok()) return Result<std::string>{ {}, r.error_code };

    // Why: 拼接顺序固定 + 用 \x1F/\x1E 作分隔符（不出现在 hex/utf8 文本里）
    std::string blob;
    blob.reserve(1024);
    for (u8 i = 0; i < (u8)HwidPart::PartCount_; ++i) {
        HwidPart p = (HwidPart)i;
        blob += partName(p); blob += '\x1F';
        blob += r.value.get(p); blob += '\x1E';
    }
    return Result<std::string>{ sha256Hex(blob), 0 };
}

std::vector<HwidPart> HwidCollector::diff(const HwidParts& a, const HwidParts& b) {
    std::vector<HwidPart> changed;
    for (u8 i = 0; i < (u8)HwidPart::PartCount_; ++i) {
        HwidPart p = (HwidPart)i;
        if (a.get(p) != b.get(p)) changed.push_back(p);
    }
    return changed;
}

}  // namespace launcher::native
