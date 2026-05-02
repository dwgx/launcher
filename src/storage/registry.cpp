#include "storage/registry.h"
#include "crypto/crypt_str.h"
#include "crypto/dpapi_seal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>

#include <spdlog/spdlog.h>
#include <random>

namespace launcher::storage {

namespace {

// 每个 slot 独立伪装路径。这些 HKCU 子键在干净 Win11 上都存在。
// Why: 单一父键放 8 个值反而显眼；分散后每键 1 个真值 + 3~5 假值，扫描需挖 8 个键。
struct SlotLoc {
    const wchar_t* (*subkeyFn)();   // CRYPT_WSTR 在每次调用时解密
    const wchar_t* value_name;      // GUID 风格
    const wchar_t* sibling_prefix;  // fake siblings 用同前缀
    int sibling_count;
};

// 用宏内联 lambda 解决 const wchar_t* 不能 constexpr 解密的问题
#define SUBKEY(literal) []() -> const wchar_t* {                          \
    static auto s = CRYPT_WSTR(literal);                                  \
    return s.c_str();                                                      \
}

const SlotLoc kLocs[] = {
    // UsernameHash → IE Cache (本机有 ZoneMap\\Cache 子键)
    { SUBKEY("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\ZoneMap\\Cache"),
      L"{B69ED7E4-3F23-4F2E-9D11-A6E5BD0F3ABF}", L"ZoneCache_", 4 },

    // PasswordToken → Office Identity (16.0 几乎所有装了 Office 的机器都有)
    { SUBKEY("Software\\Microsoft\\Office\\16.0\\Common\\Identity"),
      L"{D44F0125-87E2-4B7E-A102-7BD3D34DEEC0}", L"AdalCacheRecord_", 5 },

    // SessionToken → MS Edge per-user state（Win11 自带 Edge）
    { SUBKEY("Software\\Microsoft\\Edge\\PreferenceMACs"),
      L"{F1A2C9E5-2C73-4D08-8F62-9A6C7C5A8C19}", L"MAC_", 4 },

    // HwidBinding → Windows Search file properties cache (任何 Win11)
    { SUBKEY("Software\\Microsoft\\Windows Search\\PropertyCache\\Volume0"),
      L"{0EC5BB48-22F7-43E3-B5DD-1E1A6F1EBA34}", L"PropID_", 5 },

    // LastLoginUtc → Explorer StreamMRU (Win11 自带)
    { SUBKEY("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StreamMRU"),
      L"{8E9C5D4A-37C0-4F19-9B29-3C9F7D6F5A2E}", L"MRUListEx_", 3 },

    // SubscriptionTier → Windows Media Player Settings
    { SUBKEY("Software\\Microsoft\\MediaPlayer\\Preferences"),
      L"{2B3A7F11-9D08-44A1-9F0C-5D4F7E1C0BAA}", L"LastPlayed_", 4 },

    // SubscriptionExp → TextInputFramework (Win11 自带)
    { SUBKEY("Software\\Microsoft\\TextInputFramework\\CUI"),
      L"{6F1E0DBE-D8C4-4A1B-AB52-4B8F2E0D1FA7}", L"InputState_", 3 },

    // DeviceSeed → Shell Bags (Win11 自带, 每次开文件夹都更新)
    { SUBKEY("Software\\Microsoft\\Windows\\Shell\\BagMRU"),
      L"{C3FFB8D2-8A90-4B57-A8E2-7E9DCAFE2110}", L"NodeSlot_", 5 },
};
static_assert(sizeof(kLocs)/sizeof(kLocs[0]) == 8,
              "kLocs 数量必须等于 Slot::DeviceSeed + 1");

#undef SUBKEY

const SlotLoc& loc(Slot s) { return kLocs[(size_t)s]; }

// 创建子键并返回句柄；不存在则 RegCreateKeyEx 创建
HKEY openOrCreate(const wchar_t* sub, REGSAM access) {
    HKEY hk = nullptr;
    LSTATUS r = ::RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr,
        REG_OPTION_NON_VOLATILE, access, nullptr, &hk, nullptr);
    return r == ERROR_SUCCESS ? hk : nullptr;
}

// 写一些假数据兄弟，混淆扫描者
void sprinkle(HKEY hk, const wchar_t* prefix, int count, std::mt19937_64& rng) {
    if (!hk) return;
    for (int i = 0; i < count; ++i) {
        wchar_t name[64];
        ::swprintf_s(name, 64, L"%s%016llX", prefix, (u64)rng());
        // 已存在不覆盖
        DWORD type = 0, cb = 0;
        if (::RegQueryValueExW(hk, name, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS) continue;
        u8 fake[64];
        for (auto& b : fake) b = (u8)(rng() & 0xFF);
        ::RegSetValueExW(hk, name, 0, REG_BINARY, fake, sizeof(fake));
    }
}

}  // namespace

Registry& Registry::instance() { static Registry s; return s; }

Status Registry::writeBytes(Slot slot, const std::vector<u8>& bytes) {
    auto sealed = crypto::dpapiSeal(bytes);
    if (!sealed.ok()) return Status::Err(sealed.error_code);

    auto& l = loc(slot);
    HKEY hk = openOrCreate(l.subkeyFn(), KEY_WRITE);
    if (!hk) return Status::Err(2);

    LSTATUS r = ::RegSetValueExW(hk, l.value_name, 0, REG_BINARY,
        sealed.value.data(), (DWORD)sealed.value.size());
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS ? Status::Ok() : Status::Err((int)r);
}

Result<std::vector<u8>> Registry::readBytes(Slot slot) {
    auto& l = loc(slot);
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, l.subkeyFn(), 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return { {}, 1 };
    }
    DWORD type = 0, cb = 0;
    LSTATUS r = ::RegQueryValueExW(hk, l.value_name, nullptr, &type, nullptr, &cb);
    if (r != ERROR_SUCCESS || type != REG_BINARY || cb == 0) {
        ::RegCloseKey(hk); return { {}, 2 };
    }
    std::vector<u8> blob(cb);
    r = ::RegQueryValueExW(hk, l.value_name, nullptr, nullptr, blob.data(), &cb);
    ::RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return { {}, (int)r };
    return crypto::dpapiUnseal(blob);
}

Status Registry::erase(Slot slot) {
    auto& l = loc(slot);
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, l.subkeyFn(), 0, KEY_WRITE, &hk) != ERROR_SUCCESS)
        return Status::Err(1);
    LSTATUS r = ::RegDeleteValueW(hk, l.value_name);
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS ? Status::Ok() : Status::Err((int)r);
}

Status Registry::writeString(Slot s, const std::string& v) {
    return writeBytes(s, std::vector<u8>(v.begin(), v.end()));
}
Result<std::string> Registry::readString(Slot s) {
    auto r = readBytes(s);
    if (!r.ok()) return { {}, r.error_code };
    return { std::string(r.value.begin(), r.value.end()), 0 };
}

void Registry::wipeAll() {
    for (u8 i = 0; i <= (u8)Slot::DeviceSeed; ++i) erase((Slot)i);
    // 清掉 fake siblings：枚举每个父键，删除以 sibling_prefix 开头的值
    for (auto& l : kLocs) {
        HKEY hk = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, l.subkeyFn(), 0, KEY_WRITE | KEY_QUERY_VALUE, &hk)
                != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[256]; DWORD cn = 256, type = 0;
            LSTATUS r = ::RegEnumValueW(hk, i, name, &cn, nullptr, &type, nullptr, nullptr);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS) break;
            if (wcsncmp(name, l.sibling_prefix, wcslen(l.sibling_prefix)) == 0) {
                ::RegDeleteValueW(hk, name);
                --i;  // 删除后索引回退
            }
        }
        ::RegCloseKey(hk);
    }
}

void Registry::sprinkleDecoys() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    for (auto& l : kLocs) {
        HKEY hk = openOrCreate(l.subkeyFn(), KEY_WRITE | KEY_QUERY_VALUE);
        if (!hk) continue;
        sprinkle(hk, l.sibling_prefix, l.sibling_count, rng);
        ::RegCloseKey(hk);
    }
}

}  // namespace launcher::storage
