#pragma once

#include <SharkSDR/export.h>

#include <filesystem>
#include <string>

namespace SharkSDR {

// Folder containing the executable.
SHARKSDR_API std::filesystem::path executableDirectory();

// Per-user configuration folder (created on demand):
//   Windows: %APPDATA%\SharkSDR   macOS: ~/Library/Application Support/SharkSDR
//   Linux:   $XDG_CONFIG_HOME/SharkSDR or ~/.config/SharkSDR
// When a file named "portable" exists next to the executable, the executable
// folder is used instead.
SHARKSDR_API std::filesystem::path configDirectory();

// Default folder for user files (recordings): ~/Documents/SharkSDR or ~/SharkSDR.
SHARKSDR_API std::filesystem::path userDataDirectory();

SHARKSDR_API std::filesystem::path homeDirectory();

// UTF-8 <-> path conversion (ImGui works with UTF-8 strings).
SHARKSDR_API std::string pathToUtf8(const std::filesystem::path& path);
SHARKSDR_API std::filesystem::path utf8ToPath(const std::string& utf8);

// Overrides configDirectory() (command line --config).
SHARKSDR_API void setConfigDirectoryOverride(const std::filesystem::path& path);

}  // namespace SharkSDR
