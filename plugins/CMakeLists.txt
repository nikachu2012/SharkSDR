# Bundled plugins. Each sub folder builds one shared library into bin/plugins/.

set(SHARKSDR_BUNDLED_PLUGINS
    signal_generator
    file_source
    rtl_tcp_source
    recorder
    bookmarks
    am_sync_demod
    hello_example)

foreach(plugin IN LISTS SHARKSDR_BUNDLED_PLUGINS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${plugin}/CMakeLists.txt")
        add_subdirectory(${plugin})
    endif()
endforeach()

# Plugins kept outside of this repository:
#   cmake -B build -DSHARKSDR_EXTRA_PLUGINS="/path/to/my_plugin;/path/to/other_plugin"
# Each folder needs a CMakeLists.txt calling sharksdr_add_plugin().
set(SHARKSDR_EXTRA_PLUGINS "" CACHE STRING "Additional plugin source folders (semicolon separated)")
foreach(dir IN LISTS SHARKSDR_EXTRA_PLUGINS)
    get_filename_component(_abs "${dir}" ABSOLUTE)
    get_filename_component(_name "${_abs}" NAME)
    if(EXISTS "${_abs}/CMakeLists.txt")
        message(STATUS "SharkSDR: adding external plugin ${_abs}")
        add_subdirectory("${_abs}" "${CMAKE_BINARY_DIR}/extra_plugins/${_name}")
    else()
        message(WARNING "SharkSDR: ${_abs} has no CMakeLists.txt, ignored")
    endif()
endforeach()

# Hardware plugins: only built when the vendor library is available.
if(SHARKSDR_WITH_SOAPYSDR)
    find_package(SoapySDR CONFIG QUIET)
    if(SoapySDR_FOUND)
        message(STATUS "SharkSDR: SoapySDR found, building the SoapySDR source plugin")
        add_subdirectory(soapy_source)
    else()
        message(STATUS "SharkSDR: SoapySDR not found, skipping the SoapySDR source plugin")
    endif()
endif()

if(SHARKSDR_WITH_RTLSDR)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RTLSDR QUIET IMPORTED_TARGET librtlsdr)
    endif()
    if(NOT RTLSDR_FOUND)
        find_path(RTLSDR_INCLUDE_DIR rtl-sdr.h)
        find_library(RTLSDR_LIBRARY NAMES rtlsdr librtlsdr)
        if(RTLSDR_INCLUDE_DIR AND RTLSDR_LIBRARY)
            add_library(rtlsdr_imported UNKNOWN IMPORTED)
            set_target_properties(rtlsdr_imported PROPERTIES
                IMPORTED_LOCATION "${RTLSDR_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${RTLSDR_INCLUDE_DIR}")
            set(RTLSDR_TARGET rtlsdr_imported)
        endif()
    else()
        set(RTLSDR_TARGET PkgConfig::RTLSDR)
    endif()
    if(RTLSDR_TARGET)
        message(STATUS "SharkSDR: librtlsdr found, building the RTL-SDR source plugin")
        add_subdirectory(rtlsdr_source)
    else()
        message(STATUS "SharkSDR: librtlsdr not found, skipping the RTL-SDR source plugin")
    endif()
endif()
