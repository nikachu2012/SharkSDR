#include <SharkSDR/application.h>

#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Use UTF-8 arguments on Windows (argv is in the ANSI code page).
    std::vector<std::string> storage;
    std::vector<char*> utf8Args;
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
            storage.push_back(std::move(s));
        }
        LocalFree(wargv);
        for (auto& s : storage) utf8Args.push_back(s.data());
        utf8Args.push_back(nullptr);
        return SharkSDR::runApplication(int(storage.size()), utf8Args.data());
    }
#endif
    return SharkSDR::runApplication(argc, argv);
}
