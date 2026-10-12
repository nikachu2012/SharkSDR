#include "util/dynlib.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace SharkSDR {

#if defined(_WIN32)

namespace {
std::string lastErrorString() {
    DWORD code = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string out = "error " + std::to_string(code);
    if (msg) {
        int len = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(size_t(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, msg, -1, utf8.data(), len, nullptr, nullptr);
        while (!utf8.empty() && (utf8.back() == '\n' || utf8.back() == '\r')) utf8.pop_back();
        out += ": " + utf8;
        LocalFree(msg);
    }
    return out;
}
}  // namespace

bool DynamicLibrary::open(const std::filesystem::path& path, std::string* error) {
    close();
    // Search the plugin's own folder for its dependencies too (e.g. vendor DLLs
    // copied next to the plugin), then fall back to the standard search order.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) h = LoadLibraryW(path.c_str());
    if (!h) {
        if (error) *error = lastErrorString();
        return false;
    }
    handle_ = h;
    return true;
}

void DynamicLibrary::close() {
    if (handle_) FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const {
    if (!handle_) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
}

const char* DynamicLibrary::extension() { return ".dll"; }

#else

bool DynamicLibrary::open(const std::filesystem::path& path, std::string* error) {
    close();
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        if (error) {
            const char* e = dlerror();
            *error = e ? e : "dlopen failed";
        }
        return false;
    }
    return true;
}

void DynamicLibrary::close() {
    if (handle_) dlclose(handle_);
    handle_ = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const { return handle_ ? dlsym(handle_, name) : nullptr; }

#  if defined(__APPLE__)
const char* DynamicLibrary::extension() { return ".dylib"; }
#  else
const char* DynamicLibrary::extension() { return ".so"; }
#  endif

#endif

}  // namespace SharkSDR
