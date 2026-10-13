#include <SharkSDR/paths.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#  include <cstdlib>
#else
#  include <unistd.h>
#  include <climits>
#endif

namespace fs = std::filesystem;

namespace SharkSDR {
namespace {

std::mutex g_overrideMutex;
fs::path g_configOverride;
fs::path g_dataOverride;

fs::path envPath(const char* name) {
#if defined(_WIN32)
    std::wstring wname(name, name + std::strlen(name));
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, DWORD(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return fs::path(std::wstring(buf, n));
#else
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
#endif
}

void ensureDirectory(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
}

// base/SharkSDR. Older versions used "sharkSDR": on case-sensitive file
// systems that folder is renamed so existing settings and recordings are kept.
fs::path appFolder(const fs::path& base) {
    fs::path dir = base / "SharkSDR";
    std::error_code ec;
    if (!fs::exists(dir, ec) && fs::is_directory(base / "sharkSDR", ec)) fs::rename(base / "sharkSDR", dir, ec);
    return dir;
}

}  // namespace

fs::path executableDirectory() {
    static fs::path dir = [] {
#if defined(_WIN32)
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
            if (n < buf.size()) {
                buf.resize(n);
                break;
            }
            buf.resize(buf.size() * 2);
        }
        return fs::path(buf).parent_path();
#elif defined(__APPLE__)
        char buf[PATH_MAX];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            char real[PATH_MAX];
            if (realpath(buf, real)) return fs::path(real).parent_path();
            return fs::path(buf).parent_path();
        }
        return fs::current_path();
#else
        std::error_code ec;
        fs::path p = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path() : p.parent_path();
#endif
    }();
    return dir;
}

void setConfigDirectoryOverride(const fs::path& path) {
    std::lock_guard<std::mutex> lock(g_overrideMutex);
    g_configOverride = path;
}

fs::path configDirectory() {
    {
        std::lock_guard<std::mutex> lock(g_overrideMutex);
        if (!g_configOverride.empty()) {
            ensureDirectory(g_configOverride);
            return g_configOverride;
        }
    }
    std::error_code ec;
    if (fs::exists(executableDirectory() / "portable", ec)) return executableDirectory();

    fs::path base;
#if defined(_WIN32)
    base = envPath("APPDATA");
#elif defined(__APPLE__)
    base = homeDirectory() / "Library" / "Application Support";
#else
    base = envPath("XDG_CONFIG_HOME");
    if (base.empty()) base = homeDirectory() / ".config";
#endif
    if (base.empty()) return executableDirectory();
    fs::path dir = appFolder(base);
    ensureDirectory(dir);
    return dir;
}

fs::path homeDirectory() {
#if defined(_WIN32)
    fs::path p = envPath("USERPROFILE");
#else
    fs::path p = envPath("HOME");
#endif
    return p.empty() ? fs::current_path() : p;
}

void setUserDataDirectoryOverride(const fs::path& path) {
    std::lock_guard<std::mutex> lock(g_overrideMutex);
    g_dataOverride = path;
}

fs::path userDataDirectory() {
    {
        std::lock_guard<std::mutex> lock(g_overrideMutex);
        if (!g_dataOverride.empty()) return g_dataOverride;
    }
    std::error_code ec;
    fs::path docs = homeDirectory() / "Documents";
    return appFolder(fs::is_directory(docs, ec) ? docs : homeDirectory());
}

std::string pathToUtf8(const fs::path& path) {
#if defined(__cpp_lib_char8_t)
    auto s = path.u8string();
    return std::string(s.begin(), s.end());
#else
    return path.u8string();
#endif
}

fs::path utf8ToPath(const std::string& utf8) {
#if defined(__cpp_lib_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

}  // namespace SharkSDR
