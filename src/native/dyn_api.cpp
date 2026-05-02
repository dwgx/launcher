#include "native/dyn_api.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace launcher::native {

// Why: 不缓存 module handle，每次现拉，避免反作弊扫到稳定 handle 表
void* resolve_module(const std::string& dll_name) {
    HMODULE h = ::GetModuleHandleA(dll_name.c_str());
    if (!h) {
        h = ::LoadLibraryA(dll_name.c_str());
    }
    return reinterpret_cast<void*>(h);
}

void* resolve_proc(void* mod, const std::string& proc_name) {
    if (!mod) return nullptr;
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(mod), proc_name.c_str()));
}

}  // namespace launcher::native
