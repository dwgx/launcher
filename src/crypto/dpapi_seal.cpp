#include "crypto/dpapi_seal.h"
#include "crypto/crypt_str.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wincrypt.h>

namespace launcher::crypto {

namespace {
DATA_BLOB makeBlob(const std::vector<u8>& v) {
    DATA_BLOB b{};
    b.cbData = static_cast<DWORD>(v.size());
    b.pbData = const_cast<BYTE*>(v.data());
    return b;
}
}  // namespace

Result<std::vector<u8>> dpapiSeal(const std::vector<u8>& plain,
                                  const std::vector<u8>& entropy) {
    DATA_BLOB in  = makeBlob(plain);
    DATA_BLOB ent = makeBlob(entropy);
    DATA_BLOB out{};
    if (!::CryptProtectData(&in,
            nullptr,                              // description
            entropy.empty() ? nullptr : &ent,
            nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &out)) {
        return Result<std::vector<u8>>{ {}, static_cast<int>(::GetLastError()) };
    }
    std::vector<u8> r(out.pbData, out.pbData + out.cbData);
    ::LocalFree(out.pbData);
    return Result<std::vector<u8>>{ std::move(r), 0 };
}

Result<std::vector<u8>> dpapiUnseal(const std::vector<u8>& cipher,
                                    const std::vector<u8>& entropy) {
    DATA_BLOB in  = makeBlob(cipher);
    DATA_BLOB ent = makeBlob(entropy);
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in,
            nullptr,
            entropy.empty() ? nullptr : &ent,
            nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &out)) {
        return Result<std::vector<u8>>{ {}, static_cast<int>(::GetLastError()) };
    }
    std::vector<u8> r(out.pbData, out.pbData + out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return Result<std::vector<u8>>{ std::move(r), 0 };
}

}  // namespace launcher::crypto
