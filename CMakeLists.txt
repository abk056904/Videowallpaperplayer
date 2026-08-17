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

# ---- application ----
add_executable(VideoWallpaper WIN32
    src/app/main.cpp
    src/app/ApplicationController.cpp
    src/app/ControlWindow.cpp
    src/logging/Logger.cpp
    src/config/ConfigurationManager.cpp
    src/util/json.cpp
    src/util/utf8.cpp
)
target_include_directories(VideoWallpaper PRIVATE src)
target_link_libraries(VideoWallpaper PRIVATE user32 shell32 advapi32)
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
    src/logging/Logger.cpp
    src/config/ConfigurationManager.cpp
    src/util/json.cpp
    src/util/utf8.cpp
)
target_include_directories(vw_tests PRIVATE src tests)
target_link_libraries(vw_tests PRIVATE user32 shell32 advapi32)
target_compile_definitions(vw_tests PRIVATE ${VW_PUBLIC_DEFS})
add_test(NAME vw_tests COMMAND vw_tests)

# ---- graphics verification harness (dev-only tool, NOT shipped) ----
# Minimal windowed D3D11 harness (harness/gfx_harness.cpp) used to verify the
# M2 renderer prerequisites on this machine: device creation, debug layer,
# adapter/output enumeration, flip swap chain + vsync present, gradient render.
add_executable(vw_gfx_harness
    harness/gfx_harness.cpp
)
target_link_libraries(vw_gfx_harness PRIVATE d3d11 dxgi d3dcompiler user32)
target_compile_definitions(vw_gfx_harness PRIVATE ${VW_PUBLIC_DEFS})
if(MSVC)
    target_compile_options(vw_gfx_harness PRIVATE /WX
        $<$<CONFIG:Release>:/O2;/Zi>
        $<$<CONFIG:Debug>:/Od;/Zi;/RTC1>
    )
    target_link_options(vw_gfx_harness PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()
