cmake_minimum_required(VERSION 3.20)

project(NPPWorkSpace VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(NPPWorkSpace SHARED
    src/dllmain.cpp
    src/PluginDefinition.cpp
    src/QuickOpen.cpp
)

target_compile_definitions(NPPWorkSpace PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(NPPWorkSpace PRIVATE
    /utf-8
    /W4
    /permissive-
)

target_link_libraries(NPPWorkSpace PRIVATE
    user32
    gdi32
    shell32
    comctl32
    comdlg32
)

set_target_properties(NPPWorkSpace PROPERTIES
    OUTPUT_NAME "NPPWorkSpace"
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
