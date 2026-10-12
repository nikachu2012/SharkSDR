#pragma once

// SHARKSDR_API marks symbols exported by the core library.
// SHARKSDR_PLUGIN_EXPORT marks the C entry points exported by plugins.

#if defined(_WIN32)
#  if defined(SHARKSDR_CORE_BUILD)
#    define SHARKSDR_API __declspec(dllexport)
#  else
#    define SHARKSDR_API __declspec(dllimport)
#  endif
#  define SHARKSDR_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define SHARKSDR_API __attribute__((visibility("default")))
#  define SHARKSDR_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
