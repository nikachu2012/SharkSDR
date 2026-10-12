cmake_minimum_required(VERSION 3.20)

# std::filesystem needs macOS 10.15 or newer (ignored on other platforms).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")

project(SharkSDR
    VERSION 0.1.0
    DESCRIPTION "Cross-platform, plugin-based software defined radio receiver"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SHARKSDR_BUILD_PLUGINS "Build the bundled plugins" ON)
option(SHARKSDR_BUILD_TESTS "Build the DSP test program" ON)
option(SHARKSDR_WITH_SOAPYSDR "Build the SoapySDR source plugin when SoapySDR is found" ON)
option(SHARKSDR_WITH_RTLSDR "Build the native RTL-SDR source plugin when librtlsdr is found" ON)
option(SHARKSDR_GLFW_WAYLAND "Linux: build GLFW with Wayland support (needs the Wayland dev packages)" ON)

# Everything is placed in one relocatable folder:
#   bin/SharkSDR(.exe)           application
#   bin/SharkSDR_core.(dll|so)   core library (ImGui + DSP + plugin API)
#   bin/plugins/                 plugins loaded at runtime
#   bin/resources/               fonts
set(SHARKSDR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(SHARKSDR_PLUGIN_OUTPUT_DIR "${SHARKSDR_OUTPUT_DIR}/plugins")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SharkSDRHelpers)
include(Dependencies)

add_subdirectory(core)
add_subdirectory(app)

if(SHARKSDR_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

if(SHARKSDR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(DIRECTORY "${SHARKSDR_OUTPUT_DIR}/resources" DESTINATION .)
