#pragma once

// Plugin API.
//
// A plugin is a shared library placed in the "plugins" folder next to the
// executable (or in the user config folder). It is written like this:
//
//     #include <SharkSDR/plugin.h>
//
//     class MyPlugin : public SharkSDR::Plugin {
//     public:
//         void init(SharkSDR::Host& host) override {
//             host.registerPanel("My panel", [] { ImGui::Text("Hello!"); });
//         }
//     };
//
//     SHARKSDR_PLUGIN(MyPlugin, "My plugin", "1.0.0", "Me", "Says hello")
//
// and built with the CMake helper `sharksdr_add_plugin(my_plugin SOURCES my_plugin.cpp)`.

#include <SharkSDR/export.h>
#include <SharkSDR/host.h>

#include <imgui.h>

namespace SharkSDR {

// Bumped whenever the plugin interface changes in an incompatible way.
constexpr int kPluginApiVersion = 3;

struct PluginInfo {
    const char* name;
    const char* version;
    const char* author;
    const char* description;
};

class Plugin {
public:
    virtual ~Plugin() = default;

    // Register sources, demodulators, panels... The Host stays valid until
    // shutdown() returns.
    virtual void init(Host& host) = 0;

    // Stop all threads and release resources. Everything registered through
    // the Host is removed automatically right before this call.
    virtual void shutdown() {}
};

}  // namespace SharkSDR

// Identifies the C++ ABI / ImGui combination: plugins are C++ libraries that
// share objects with the application, so they must use a compatible C++ ABI
// (and, with MSVC, the same debug/release runtime) and the same ImGui version.
#define SHARKSDR_STRINGIFY_(x) #x
#define SHARKSDR_STRINGIFY(x) SHARKSDR_STRINGIFY_(x)
#if defined(_MSC_VER)
#  if defined(_DEBUG)
#    define SHARKSDR_ABI_ID "msvc-debug"
#  else
#    define SHARKSDR_ABI_ID "msvc-release"
#  endif
#elif defined(_LIBCPP_VERSION)
#  define SHARKSDR_ABI_ID "libc++-abi" SHARKSDR_STRINGIFY(_LIBCPP_ABI_VERSION)
#elif defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI
#  define SHARKSDR_ABI_ID "libstdc++-cxx11"
#elif defined(__GLIBCXX__)
#  define SHARKSDR_ABI_ID "libstdc++-old"
#else
#  define SHARKSDR_ABI_ID "unknown"
#endif
#define SHARKSDR_BUILD_SIGNATURE SHARKSDR_ABI_ID "/imgui-" IMGUI_VERSION

// Defines the C entry points the application looks for.
#define SHARKSDR_PLUGIN(PluginClass, Name, Version, Author, Description)                         \
    extern "C" SHARKSDR_PLUGIN_EXPORT int sharksdr_plugin_api_version() {                           \
        return ::SharkSDR::kPluginApiVersion;                                                     \
    }                                                                                          \
    extern "C" SHARKSDR_PLUGIN_EXPORT const char* sharksdr_plugin_build_signature() {               \
        return SHARKSDR_BUILD_SIGNATURE;                                                          \
    }                                                                                          \
    extern "C" SHARKSDR_PLUGIN_EXPORT const ::SharkSDR::PluginInfo* sharksdr_plugin_info() {           \
        static const ::SharkSDR::PluginInfo info{Name, Version, Author, Description};             \
        return &info;                                                                          \
    }                                                                                          \
    extern "C" SHARKSDR_PLUGIN_EXPORT ::SharkSDR::Plugin* sharksdr_plugin_create() {                   \
        return new PluginClass();                                                              \
    }                                                                                          \
    extern "C" SHARKSDR_PLUGIN_EXPORT void sharksdr_plugin_destroy(::SharkSDR::Plugin* plugin) {       \
        delete plugin;                                                                         \
    }
