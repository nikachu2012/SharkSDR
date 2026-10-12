sharksdr_add_plugin(rtlsdr_source SOURCES rtlsdr_source.cpp LIBRARIES ${RTLSDR_TARGET})

# rtlsdr_set_bias_tee() only exists in librtlsdr >= 0.6.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES ${RTLSDR_TARGET})
check_cxx_source_compiles("
#include <rtl-sdr.h>
int main() { return rtlsdr_set_bias_tee(nullptr, 0); }" SHARKSDR_RTLSDR_HAS_BIAS_TEE)
unset(CMAKE_REQUIRED_LIBRARIES)
if(SHARKSDR_RTLSDR_HAS_BIAS_TEE)
    target_compile_definitions(rtlsdr_source PRIVATE SHARKSDR_RTLSDR_BIAS_TEE=1)
endif()
