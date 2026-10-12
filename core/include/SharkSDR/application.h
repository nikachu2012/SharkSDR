#pragma once

#include <SharkSDR/export.h>

namespace SharkSDR {

SHARKSDR_API const char* versionString();

// Runs the application until the main window is closed. Returns the exit code.
SHARKSDR_API int runApplication(int argc, char** argv);

}  // namespace SharkSDR
