# SharkSDR_core: shared library containing Dear ImGui, the DSP building blocks,
# the plugin API and the application itself. The executable is a thin wrapper
# and every plugin links against this library, so all of them share a single
# ImGui context and the same DSP code.

set(IMGUI_DIR "${imgui_SOURCE_DIR}")

add_library(SharkSDR_core SHARED
    # Dear ImGui
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp

    # Utilities
    src/log.cpp
    src/paths.cpp
    src/util/net.cpp
    src/util/wav.cpp
    src/util/dynlib.cpp

    # DSP
    src/dsp/fft.cpp
    src/dsp/window.cpp
    src/dsp/filter.cpp
    src/dsp/demodulators.cpp
    src/dsp/noise_reduction.cpp
    src/dsp/receiver.cpp

    # UI helpers available to plugins
    src/ui/widgets.cpp
    src/ui/file_dialog.cpp
    src/ui/texture.cpp
    src/ui/theme.cpp

    # Application
    src/app/application.cpp
    src/app/audio_output.cpp
    src/app/colormap.cpp
    src/app/engine.cpp
    src/app/host_impl.cpp
    src/app/miniaudio_impl.cpp
    src/app/plugin_manager.cpp
    src/app/png_writer.cpp
    src/app/spectrum_view.cpp
)

target_include_directories(SharkSDR_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${IMGUI_DIR}>
        $<BUILD_INTERFACE:${IMGUI_DIR}/misc/cpp>
    PRIVATE
        src
        ${IMGUI_DIR}/backends
        ${miniaudio_SOURCE_DIR})

target_compile_features(SharkSDR_core PUBLIC cxx_std_17)  # also for plugins built against the SDK
target_compile_definitions(SharkSDR_core
    PUBLIC IMGUI_DEFINE_MATH_OPERATORS
    PRIVATE SHARKSDR_CORE_BUILD SHARKSDR_VERSION_STRING="${PROJECT_VERSION}")

if(WIN32)
    # Export ImGui from the DLL so that plugins use the application's context.
    target_compile_definitions(SharkSDR_core
        PRIVATE "IMGUI_API=__declspec(dllexport)"
        INTERFACE "IMGUI_API=__declspec(dllimport)")
endif()

target_link_libraries(SharkSDR_core
    PUBLIC nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE glfw OpenGL::GL ${CMAKE_DL_LIBS})

if(WIN32)
    target_link_libraries(SharkSDR_core PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(SharkSDR_core PRIVATE
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
else()
    target_link_libraries(SharkSDR_core PRIVATE m)
endif()

sharksdr_set_compile_options(SharkSDR_core)
sharksdr_set_output_dir(SharkSDR_core "${SHARKSDR_OUTPUT_DIR}")
sharksdr_set_rpath(SharkSDR_core "")

# Fonts used by the UI.
file(COPY
    ${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf
    ${IMGUI_DIR}/misc/fonts/Cousine-Regular.ttf
    DESTINATION "${SHARKSDR_OUTPUT_DIR}/resources/fonts")

install(TARGETS SharkSDR_core RUNTIME DESTINATION . LIBRARY DESTINATION .)
