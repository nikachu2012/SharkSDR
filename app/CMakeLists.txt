add_executable(SharkSDR main.cpp)
target_link_libraries(SharkSDR PRIVATE SharkSDR_core)
sharksdr_set_compile_options(SharkSDR)
sharksdr_set_output_dir(SharkSDR "${SHARKSDR_OUTPUT_DIR}")
sharksdr_set_rpath(SharkSDR "")

if(WIN32)
    # GUI application without a console window in release builds.
    set_target_properties(SharkSDR PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release,MinSizeRel>)
    if(MSVC)
        target_link_options(SharkSDR PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

install(TARGETS SharkSDR RUNTIME DESTINATION .)
