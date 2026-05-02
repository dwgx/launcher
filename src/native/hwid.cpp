#include "native/hwid.h"
#include "crypto/crypt_str.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WbemIdl.h>
#include <comdef.h>
#include <comutil.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <sodium.h>

#include <spdlog/spdlog.h>
#include <vector>
#include <array>
#include <cstring>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace launcher::native {

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string toHex(const u8* data, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[(data[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

// 单例 COM/WMI 会话；首次调用时初始化
class WmiSession {
public:
    static WmiSession& instance() { static WmiSession s; return s; }

    IWbemServices* services() { return m_svc; }
    bool ok() const { return m_ok; }

    ~WmiSession() {
        if (m_svc) m_svc->Release();
        if (m_loc) m_loc->Release();
        if (m_co_init) ::CoUninitialize();
    }

private:
    WmiSession() {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr != S_OK && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            spdlog::warn("CoInitializeEx failed 0x{:X}", static_cast<u32>(hr));
            return;
        }
        m_co_init = (hr != RPC_E_CHANGED_MODE);
        // CoInitializeSecurity 必须最先调用且只能一次；
        // 若进程已设置过会返回 RPC_E_TOO_LATE，忽略即可
        ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                               RPC_C_AUTHN_LEVEL_DEFAULT,
                               RPC_C_IMP_LEVEL_IMPERSONATE,
                               nullptr, EOAC_NONE, nullptr);

        hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void**>(&m_loc));
        if (FAILED(hr)) return;

        BSTR ns = ::SysAllocString(L"ROOT\\CIMV2");
        hr = m_loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0,
                                  nullptr, nullptr, &m_svc);
        ::SysFreeString(ns);
        if (FAILED(hr)) { m_svc = nullptr; return; }

        ::CoSetProxyBlanket(m_svc,
            RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE);
        m_ok = true;
    }
    bool m_co_init{false};
    bool m_ok{false};
    IWbemLocator*  m_loc{nullptr};
    IWbemServices* m_svc{nullptr};
};

std::string wmiQuerySingle(const wchar_t* wql, const wchar_t* prop) {
    auto& s = WmiSession::instance();
    if (!s.ok()) return {};

    IEnumWbemClassObject* en = nullptr;
    BSTR lang  = ::SysAllocString(L"WQL");
    BSTR query = ::SysAllocString(wql);
    HRESULT hr = s.services()->ExecQuery(lang, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    ::SysFreeString(lang);
    ::SysFreeString(query);
    if (FAILED(hr) || !en) return {};

    std::string out;
    IWbemClassObject* obj = nullptr;
    ULONG ret = 0;
    while (en->Next(WBEM_INFINITE, 1, &obj, &ret) == WBEM_S_NO_ERROR && ret) {
        VARIANT v; ::VariantInit(&v);
        if (obj->Get(prop, 0, &v, nullptr, nullptr) == S_OK
                && v.vt == VT_BSTR && v.bstrVal) {
            out = toUtf8(v.bstrVal);
        }
        ::VariantClear(&v);
        obj->Release(); obj = nullptr;
        if (!out.empty()) break;
    }
    en->Release();
    return trim(out);
}

std::string queryMachineGuid() {
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Cryptography",
            0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS) return {};
    wchar_t buf[128] = {0};
    DWORD cb = sizeof(buf);
    LSTATUS r = ::RegQueryValueExW(hk, L"MachineGuid", nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buf), &cb);
    ::RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return {};
    return toUtf8(std::wstring(buf));
}

std::string queryVolumeSerial() {
    DWORD serial = 0;
    if (!::GetVolumeInformationW(L"C:\\", nullptr, 0, &serial,
                                  nullptr, nullptr, nullptr, 0)) return {};
    char b[16];
    ::sprintf_s(b, "%08X", serial);
    return std::string(b);
}

std::string queryPrimaryMac() {
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

    // 选第一个 PhysicalAddressLength == 6 且 IfType 是 Ethernet/WiFi 且非 loopback
    for (auto* a = head; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->PhysicalAddressLength != 6) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        char mac[18];
        ::sprintf_s(mac, "%02X-%02X-%02X-%02X-%02X-%02X",
                    a->PhysicalAddress[0], a->PhysicalAddress[1],
                    a->PhysicalAddress[2], a->PhysicalAddress[3],
                    a->PhysicalAddress[4], a->PhysicalAddress[5]);
        return std::string(mac);
    }
    return {};
}

std::string queryCpuSignature() {
    int regs[4] = {0};
    char brand[0x40] = {0};
    ::__cpuid(regs, 0x80000000);
    unsigned max_ext = static_cast<unsigned>(regs[0]);
    if (max_ext >= 0x80000004) {
        ::__cpuid(reinterpret_cast<int*>(brand),       0x80000002);
        ::__cpuid(reinterpret_cast<int*>(brand + 16),  0x80000003);
        ::__cpuid(reinterpret_cast<int*>(brand + 32),  0x80000004);
    }
    ::__cpuid(regs, 1);
    char b[96];
    ::sprintf_s(b, "%s|%08X-%08X", trim(brand).c_str(),
                static_cast<u32>(regs[0]), static_cast<u32>(regs[3]));
    return std::string(b);
}

}  // namespace

bool HwidParts::isUsable() const {
    int filled = 0;
    if (!baseboard_serial.empty()) ++filled;
    if (!bios_uuid.empty())        ++filled;
    if (!cpu_sig.empty())          ++filled;
    if (!volume_serial.empty())    ++filled;
    if (!primary_mac.empty())      ++filled;
    if (!gpu_pnp.empty())          ++filled;
    if (!machine_guid.empty())     ++filled;
    return filled >= 4;
}

Result<HwidParts> HwidCollector::collectParts() {
    HwidParts p;
    p.baseboard_serial = wmiQuerySingle(L"SELECT SerialNumber FROM Win32_BaseBoard",
                                        L"SerialNumber");
    p.bios_uuid        = wmiQuerySingle(L"SELECT UUID FROM Win32_ComputerSystemProduct",
                                        L"UUID");
    p.gpu_pnp          = wmiQuerySingle(L"SELECT PNPDeviceID FROM Win32_VideoController",
                                        L"PNPDeviceID");
    p.cpu_sig          = queryCpuSignature();
    p.volume_serial    = queryVolumeSerial();
    p.primary_mac      = queryPrimaryMac();
    p.machine_guid     = queryMachineGuid();

    if (!p.isUsable()) {
        spdlog::warn("HwidParts has fewer than 4 usable fields");
        return Result<HwidParts>{ p, 1 };
    }
    return Result<HwidParts>{ p, 0 };
}

Result<std::string> HwidCollector::collectFingerprint() {
    auto r = collectParts();
    if (!r.ok()) return Result<std::string>{ {}, r.error_code };

    // Why: 拼接顺序固定 + 分隔符不可在内容中出现，避免歧义
    std::string blob;
    blob.reserve(512);
    auto append = [&](const std::string& tag, const std::string& v) {
        blob += tag; blob += '\x1F'; blob += v; blob += '\x1E';
    };
    append("bb", r.value.baseboard_serial);
    append("bu", r.value.bios_uuid);
    append("cp", r.value.cpu_sig);
    append("vs", r.value.volume_serial);
    append("mc", r.value.primary_mac);
    append("gp", r.value.gpu_pnp);
    append("mg", r.value.machine_guid);

    if (sodium_init() < 0) return Result<std::string>{ {}, 2 };
    u8 hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash,
                       reinterpret_cast<const u8*>(blob.data()),
                       blob.size());
    return Result<std::string>{ toHex(hash, sizeof(hash)), 0 };
}

}  // namespace launcher::native
