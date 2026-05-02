#pragma once

// 动态导入 Win32 API：避开静态 IAT 让加壳后扫描更难。
// 用法：
//   auto pCreateProcessW = dyn_api::resolve<BOOL(WINAPI*)(...)>(
//       CRYPT_STR("kernel32.dll"), CRYPT_STR("CreateProcessW"));

#include "app/common.h"
#include "crypto/crypt_str.h"

namespace launcher::native {

void* resolve_module(const std::string& dll_name);
void* resolve_proc(void* mod, const std::string& proc_name);

template <typename Fn>
inline Fn resolve(const std::string& dll_name, const std::string& proc_name) {
    void* m = resolve_module(dll_name);
    if (!m) return nullptr;
    return reinterpret_cast<Fn>(resolve_proc(m, proc_name));
}

}  // namespace launcher::native
