# Helper functions shared by the core, the application and the plugins.

# Common warning / optimisation flags.
function(sharksdr_set_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /utf-8 /wd4251 /wd4275 /fp:fast /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter
            $<$<CONFIG:Release,RelWithDebInfo>:-O3> -fno-math-errno
            $<$<CXX_COMPILER_ID:GNU>:-fcx-limited-range>)
    endif()
endfunction()

# Put a target's binaries into a fixed folder (no per-config sub folders).
function(sharksdr_set_output_dir target dir)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${dir}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${dir}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/lib>")
endfunction()

# Runtime search path so that the relocatable bin/ folder works on Linux/macOS.
#   relative = "" for files in bin/, "/.." for files in bin/plugins/
function(sharksdr_set_rpath target relative)
    if(APPLE)
        set(_rp "@loader_path${relative}")
    elseif(UNIX)
        set(_rp "$ORIGIN${relative}")
    else()
        return()
    endif()
    set_target_properties(${target} PROPERTIES BUILD_RPATH "${_rp}" INSTALL_RPATH "${_rp}")
endfunction()

# Declare a SharkSDR plugin.
#
#   sharksdr_add_plugin(my_plugin SOURCES a.cpp b.cpp [LIBRARIES foo])
#
# The plugin is a shared library placed in bin/plugins/ that links against the
# core library (which provides ImGui, the DSP helpers and the plugin API).
function(sharksdr_add_plugin name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
    add_library(${name} MODULE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE SharkSDR_core ${ARG_LIBRARIES})
    set_target_properties(${name} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
    sharksdr_set_compile_options(${name})
    sharksdr_set_output_dir(${name} "${SHARKSDR_PLUGIN_OUTPUT_DIR}")
    # Find the core library one level up and vendor libraries shipped next to
    # the plugin; keep the link paths of external SDKs (e.g. /opt) when installed.
    if(APPLE)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "@loader_path;@loader_path/.." INSTALL_RPATH "@loader_path;@loader_path/.."
            INSTALL_RPATH_USE_LINK_PATH TRUE)
    elseif(UNIX)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH "$ORIGIN;$ORIGIN/.." INSTALL_RPATH "$ORIGIN;$ORIGIN/.."
            INSTALL_RPATH_USE_LINK_PATH TRUE)
    endif()
    install(TARGETS ${name} LIBRARY DESTINATION plugins RUNTIME DESTINATION plugins)
endfunction()
