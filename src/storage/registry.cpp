#include "storage/registry.h"
#include "crypto/crypt_str.h"
#include "crypto/dpapi_seal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <spdlog/spdlog.h>
#include <array>

namespace launcher::storage {

namespace {

// Why: 把数据藏在"看起来很系统"的子键里，单字段值名也用 GUID 风格混进默认表
// 实际路径在运行时由 CRYPT_STR 解密，反编译看到的是密文
std::wstring slotSubkey(Slot) {
    return CRYPT_WSTR("Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings\\ZoneMap\\Cache");
}

const wchar_t* slotValueName(Slot s) {
    // GUID-like 名字。Why: 看着像系统遗留键值，扫眼不会注意
    static const wchar_t* kNames[] = {
        L"{B69ED7E4-3F23-4F2E-9D11-A6E5BD0F3ABF}",  // UsernameHash
        L"{D44F0125-87E2-4B7E-A102-7BD3D34DEEC0}",  // PasswordToken
        L"{F1A2C9E5-2C73-4D08-8F62-9A6C7C5A8C19}",  // SessionToken
        L"{0EC5BB48-22F7-43E3-B5DD-1E1A6F1EBA34}",  // HwidBinding
        L"{8E9C5D4A-37C0-4F19-9B29-3C9F7D6F5A2E}",  // LastLoginUtc
        L"{2B3A7F11-9D08-44A1-9F0C-5D4F7E1C0BAA}",  // SubscriptionTier
        L"{6F1E0DBE-D8C4-4A1B-AB52-4B8F2E0D1FA7}",  // SubscriptionExp
    };
    return kNames[static_cast<size_t>(s)];
}

}  // namespace

Registry& Registry::instance() { static Registry s; return s; }

Status Registry::writeBytes(Slot slot, const std::vector<u8>& bytes) {
    auto sealed = crypto::dpapiSeal(bytes);
    if (!sealed.ok()) {
        spdlog::error("dpapiSeal failed code={}", sealed.error_code);
        return Status::Err(sealed.error_code);
    }

    HKEY hk = nullptr;
    auto path = slotSubkey(slot);
    LSTATUS r = ::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                  &hk, nullptr);
    if (r != ERROR_SUCCESS) return Status::Err(static_cast<int>(r));

    r = ::RegSetValueExW(hk, slotValueName(slot), 0, REG_BINARY,
                         sealed.value.data(),
                         static_cast<DWORD>(sealed.value.size()));
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS ? Status::Ok() : Status::Err(static_cast<int>(r));
}

Result<std::vector<u8>> Registry::readBytes(Slot slot) {
    HKEY hk = nullptr;
    auto path = slotSubkey(slot);
    LSTATUS r = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &hk);
    if (r != ERROR_SUCCESS) return { {}, static_cast<int>(r) };

    DWORD type = 0, cb = 0;
    r = ::RegQueryValueExW(hk, slotValueName(slot), nullptr, &type, nullptr, &cb);
    if (r != ERROR_SUCCESS || type != REG_BINARY || cb == 0) {
        ::RegCloseKey(hk);
        return { {}, static_cast<int>(r) };
    }
    std::vector<u8> blob(cb);
    r = ::RegQueryValueExW(hk, slotValueName(slot), nullptr, nullptr,
                           blob.data(), &cb);
    ::RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return { {}, static_cast<int>(r) };

    return crypto::dpapiUnseal(blob);
}

Status Registry::erase(Slot slot) {
    HKEY hk = nullptr;
    auto path = slotSubkey(slot);
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_WRITE, &hk)
            != ERROR_SUCCESS) {
        return Status::Err(1);
    }
    LSTATUS r = ::RegDeleteValueW(hk, slotValueName(slot));
    ::RegCloseKey(hk);
    return r == ERROR_SUCCESS ? Status::Ok() : Status::Err(static_cast<int>(r));
}

Status Registry::writeString(Slot slot, const std::string& s) {
    return writeBytes(slot, std::vector<u8>(s.begin(), s.end()));
}

Result<std::string> Registry::readString(Slot slot) {
    auto r = readBytes(slot);
    if (!r.ok()) return { {}, r.error_code };
    return { std::string(r.value.begin(), r.value.end()), 0 };
}

}  // namespace launcher::storage
