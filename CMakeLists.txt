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
)
target_include_directories(VideoWallpaper PRIVATE src)
target_link_libraries(VideoWallpaper PRIVATE user32 shell32 advapi32)
target_compile_definitions(VideoWallpaper PRIVATE
    ${VW_PUBLIC_DEFS}
    $<$<CONFIG:Debug>:VW_DEBUG>   # debug layer / verbose logging only in Debug
)

if(MSVC)
    target_compile_options(VideoWallpaper PRIVATE
        $<$<CONFIG:Release>:/O2;/GL>
        $<$<CONFIG:Debug>:/Od;/Zi>
    )
    target_link_options(VideoWallpaper PRIVATE
        $<$<CONFIG:Release>:/LTCG>
    )
endif()

# ---- unit tests (doctest, test-only; never shipped) ----
enable_testing()
add_executable(vw_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_config.cpp
    tests/test_logger.cpp
    src/logging/Logger.cpp
    src/config/ConfigurationManager.cpp
    src/util/json.cpp
)
target_include_directories(vw_tests PRIVATE src tests)
target_link_libraries(vw_tests PRIVATE user32 shell32 advapi32)
target_compile_definitions(vw_tests PRIVATE ${VW_PUBLIC_DEFS})
add_test(NAME vw_tests COMMAND vw_tests)
