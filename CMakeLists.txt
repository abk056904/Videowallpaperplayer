cmake_minimum_required(VERSION 3.25)

project(VideoWallpaper
    VERSION 0.1.0
    DESCRIPTION "Native Windows 11 video wallpaper engine"
    LANGUAGES CXX)

# ---- global settings ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()

# Common defines for all targets (native Win32, wide strings).
set(VW_PUBLIC_DEFS NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)

# ---- build-time shader compilation (fxc) + embedding ----
# shaders/VideoShader.hlsl is compiled to .cso at build time and embedded as
# C++ byte arrays so the exe ships shaders with no runtime file lookups
# (M14 package = exe + system DLLs only).
set(FXC_EXE "")
if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(_fxc_candidate "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64/fxc.exe")
    if(EXISTS "${_fxc_candidate}")
        set(FXC_EXE "${_fxc_candidate}")
    endif()
endif()
if(NOT FXC_EXE)
    file(GLOB _fxc_glob "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
    list(LENGTH _fxc_glob _fxc_count)
    if(_fxc_count GREATER 0)
        list(GET _fxc_glob 0 FXC_EXE)
    endif()
endif()
if(NOT FXC_EXE)
    message(FATAL_ERROR "fxc.exe not found in the Windows SDK — build-time shader compilation (M2) cannot proceed")
endif()
message(STATUS "fxc: ${FXC_EXE}")

set(_shader_src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/VideoShader.hlsl)
set(_shader_outdir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(_vs_cso ${_shader_outdir}/VideoShader.vs.cso)
set(_ps_cso ${_shader_outdir}/VideoShader.ps.cso)
set(_ps_tex_cso ${_shader_outdir}/VideoShader.ps_tex.cso)
set(_ps_yuv_cso ${_shader_outdir}/VideoShader.ps_yuv.cso)
set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_vs_hdr ${_gen_dir}/VideoShaderVsData.h)
set(_ps_hdr ${_gen_dir}/VideoShaderPsData.h)
set(_ps_tex_hdr ${_gen_dir}/VideoShaderPsTexData.h)
set(_ps_yuv_hdr ${_gen_dir}/VideoShaderPsYuvData.h)

add_custom_command(
    OUTPUT ${_vs_cso} ${_ps_cso} ${_ps_tex_cso} ${_ps_yuv_cso}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_shader_outdir}
    COMMAND ${FXC_EXE} /nologo /T vs_5_0 /E VSMain /Fo ${_vs_cso} ${_shader_src}
    COMMAND ${FXC_EXE} /nologo /T ps_5_0 /E PSMain /Fo ${_ps_cso} ${_shader_src}
    COMMAND ${FXC_EXE} /nologo /T ps_5_0 /E PSMainTexture /Fo ${_ps_tex_cso} ${_shader_src}
    COMMAND ${FXC_EXE} /nologo /T ps_5_0 /E PSMainYuv /Fo ${_ps_yuv_cso} ${_shader_src}
    DEPENDS ${_shader_src}
    COMMENT "Compiling VideoShader.hlsl with fxc"
    VERBATIM
)
add_custom_command(
    OUTPUT ${_vs_hdr}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_vs_cso} -DOUTPUT=${_vs_hdr} -DNAME=kVideoShaderVs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    DEPENDS ${_vs_cso} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    COMMENT "Embedding vertex shader"
    VERBATIM
)
add_custom_command(
    OUTPUT ${_ps_hdr}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_ps_cso} -DOUTPUT=${_ps_hdr} -DNAME=kVideoShaderPs
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    DEPENDS ${_ps_cso} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    COMMENT "Embedding pixel shader"
    VERBATIM
)
add_custom_command(
    OUTPUT ${_ps_tex_hdr}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_ps_tex_cso} -DOUTPUT=${_ps_tex_hdr} -DNAME=kVideoShaderPsTex
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    DEPENDS ${_ps_tex_cso} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    COMMENT "Embedding textured pixel shader"
    VERBATIM
)
add_custom_command(
    OUTPUT ${_ps_yuv_hdr}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_ps_yuv_cso} -DOUTPUT=${_ps_yuv_hdr} -DNAME=kVideoShaderPsYuv
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    DEPENDS ${_ps_yuv_cso} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake
    COMMENT "Embedding YUV pixel shader"
    VERBATIM
)
set_source_files_properties(${_vs_hdr} ${_ps_hdr} ${_ps_tex_hdr} ${_ps_yuv_hdr} PROPERTIES GENERATED TRUE)

# Graphics module sources shared by the app and the harness.
set(VW_GRAPHICS_SOURCES
    src/graphics/D3D11DeviceManager.cpp
    src/graphics/D3D11Renderer.cpp
    src/graphics/TextureManager.cpp
)

# Video module sources (M4: Media Foundation playback, software first).
set(VW_VIDEO_SOURCES
    src/video/VideoMetadata.cpp
    src/video/FrameQueue.cpp
    src/video/DecoderManager.cpp
    src/video/VideoPlayer.cpp
)

# Playback module sources (M6: frame timing, queue & scheduling).
set(VW_PLAYBACK_SOURCES
    src/playback/FrameScheduler.cpp
    src/playback/PlaybackController.cpp
)

# Performance module sources (M6→M9: telemetry aggregation + workload sampling).
set(VW_PERFORMANCE_SOURCES
    src/performance/StatsCollector.cpp
    src/performance/WorkloadMonitor.cpp
)

# Detection module sources (M9: game + fullscreen classification).
set(VW_DETECTION_SOURCES
    src/detection/GameDetector.cpp
)

# Playlist module sources (M7: playlist engine + persistence).
set(VW_PLAYLIST_SOURCES
    src/playlist/PlaylistManager.cpp
    src/playlist/PlaylistStore.cpp
)

# Media Foundation libraries (M4+; evr provides MR_VIDEO_ACCELERATION_SERVICE).
set(VW_MF_LIBS mfplat mfreadwrite mfuuid mf evr)

# ---- application ----
add_executable(VideoWallpaper WIN32
    src/app/main.cpp
    src/app/ApplicationController.cpp
    src/app/ControlWindow.cpp
    src/app/app.rc   # embeds the application icon (video_wallpaper_engine_compact_rgb.ico)
    src/logging/Logger.cpp
    src/config/ConfigurationManager.cpp
    src/monitors/MonitorManager.cpp
    src/wallpaper/WallpaperHost.cpp
    src/wallpaper/WallpaperManager.cpp
    src/util/json.cpp
    src/util/utf8.cpp
    ${VW_GRAPHICS_SOURCES}
    ${VW_VIDEO_SOURCES}
    ${VW_PLAYBACK_SOURCES}
    ${VW_PERFORMANCE_SOURCES}
    ${VW_DETECTION_SOURCES}
    ${VW_PLAYLIST_SOURCES}
    ${_vs_hdr} ${_ps_hdr} ${_ps_tex_hdr} ${_ps_yuv_hdr}
)
target_include_directories(VideoWallpaper PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(VideoWallpaper PRIVATE user32 shell32 advapi32 psapi d3d11 dxgi ${VW_MF_LIBS})
target_compile_definitions(VideoWallpaper PRIVATE
    ${VW_PUBLIC_DEFS}
    $<$<CONFIG:Debug>:VW_DEBUG>   # debug layer / verbose logging only in Debug
)

if(MSVC)
    target_compile_options(VideoWallpaper PRIVATE /WX # warnings-as-errors (plan: CI-style)
        $<$<CONFIG:Release>:/O2;/GL;/Zi>
        $<$<CONFIG:Debug>:/Od;/Zi;/RTC1>
    )
    target_link_options(VideoWallpaper PRIVATE
        $<$<CONFIG:Release>:/LTCG;/DEBUG>   # PDBs for future crash dumps
    )
endif()

# ---- unit tests (doctest, test-only; never shipped) ----
enable_testing()
add_executable(vw_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_config.cpp
    tests/test_logger.cpp
    tests/test_utf8.cpp
    tests/test_graphics.cpp
    tests/test_monitors.cpp
    tests/test_playback.cpp
    tests/test_playlist.cpp
    tests/test_scale.cpp
    tests/test_stats.cpp
    tests/test_detection.cpp
    tests/test_video.cpp
    src/logging/Logger.cpp
    src/config/ConfigurationManager.cpp
    src/monitors/MonitorManager.cpp
    src/util/json.cpp
    src/util/utf8.cpp
    src/graphics/D3D11DeviceManager.cpp
    src/graphics/TextureManager.cpp
    ${VW_VIDEO_SOURCES}
    ${VW_PLAYBACK_SOURCES}
    ${VW_PERFORMANCE_SOURCES}
    ${VW_DETECTION_SOURCES}
    ${VW_PLAYLIST_SOURCES}
)
target_include_directories(vw_tests PRIVATE src tests)
target_link_libraries(vw_tests PRIVATE user32 shell32 advapi32 d3d11 dxgi ${VW_MF_LIBS})
target_compile_definitions(vw_tests PRIVATE ${VW_PUBLIC_DEFS})
add_test(NAME vw_tests COMMAND vw_tests)

# ---- graphics verification harness (dev-only tool, NOT shipped) ----
# Minimal windowed D3D11 harness (harness/gfx_harness.cpp) used to verify the
# M2 renderer prerequisites on this machine: device creation, debug layer,
# adapter/output enumeration, flip swap chain + vsync present, gradient render.
add_executable(vw_gfx_harness
    harness/gfx_harness.cpp
    src/logging/Logger.cpp
    src/monitors/MonitorManager.cpp
    src/wallpaper/WallpaperHost.cpp
    src/wallpaper/WallpaperManager.cpp
    src/util/utf8.cpp
    ${VW_GRAPHICS_SOURCES}
    ${_vs_hdr} ${_ps_hdr} ${_ps_tex_hdr} ${_ps_yuv_hdr}
)
target_include_directories(vw_gfx_harness PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vw_gfx_harness PRIVATE d3d11 dxgi user32 mfplat mfreadwrite mfuuid)
target_compile_definitions(vw_gfx_harness PRIVATE
    ${VW_PUBLIC_DEFS}
    $<$<CONFIG:Debug>:VW_DEBUG>   # same Debug marker as the app
)
if(MSVC)
    target_compile_options(vw_gfx_harness PRIVATE /WX
        $<$<CONFIG:Release>:/O2;/Zi>
        $<$<CONFIG:Debug>:/Od;/Zi;/RTC1>
    )
    target_link_options(vw_gfx_harness PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()
