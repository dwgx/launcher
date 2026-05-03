// 真 HWID — 1:1 复刻 tools/preview/loading_demo.cpp::hwidHex。
// ComputerName + UserName + 系统盘 VolumeSerial → SHA-256 → 64 hex 字符。
// 后端 /api/auth/{login,register} 强制 64 字。正式版 14 个硬件源在 src/native/hwid。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>
#include <string>
#include <cstdio>

namespace launcher::d2d {

inline std::string hwidHex() {
    static std::string cached;
    if (!cached.empty()) return cached;

    std::wstring blob;
    wchar_t cn[256] = {0}; DWORD cnsz = 256;
    if (GetComputerNameW(cn, &cnsz)) blob.append(cn, cnsz);
    blob.push_back(L'|');
    wchar_t un[256] = {0}; DWORD unsz = 256;
    if (GetUserNameW(un, &unsz)) blob.append(un, unsz - 1);
    blob.push_back(L'|');
    DWORD volSerial = 0;
    if (GetVolumeInformationW(L"C:\\", nullptr, 0, &volSerial,
                              nullptr, nullptr, nullptr, 0)) {
        wchar_t vs[16]; swprintf_s(vs, 16, L"%08X", volSerial);
        blob.append(vs);
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, blob.c_str(), (int)blob.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string utf8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, blob.c_str(), (int)blob.size(),
                        utf8.data(), n, nullptr, nullptr);

    HCRYPTPROV prov = 0; HCRYPTHASH h = 0;
    BYTE hash[32] = {0};
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)) {
            CryptHashData(h, (const BYTE*)utf8.data(), (DWORD)utf8.size(), 0);
            DWORD hsz = 32;
            CryptGetHashParam(h, HP_HASHVAL, hash, &hsz, 0);
            CryptDestroyHash(h);
        }
        CryptReleaseContext(prov, 0);
    }
    char out[65] = {0};
    for (int i = 0; i < 32; ++i) sprintf_s(out + i * 2, 3, "%02x", hash[i]);
    cached.assign(out, 64);
    return cached;
}

}  // namespace launcher::d2d
