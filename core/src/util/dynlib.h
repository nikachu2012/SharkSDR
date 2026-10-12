#pragma once

#include <filesystem>
#include <string>

namespace SharkSDR {

// Thin wrapper around LoadLibrary / dlopen.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary() { close(); }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool open(const std::filesystem::path& path, std::string* error);
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    void* symbol(const char* name) const;

    template <typename F>
    F function(const char* name) const {
        return reinterpret_cast<F>(symbol(name));
    }

    static const char* extension();  // ".dll", ".so" or ".dylib"

private:
    void* handle_ = nullptr;
};

}  // namespace SharkSDR
