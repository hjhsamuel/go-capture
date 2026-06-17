cmake_minimum_required(VERSION 3.15)
project(CaptureLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable C++/WinRT
add_compile_options(/await /std:c++20)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(CaptureLib STATIC
    CaptureLib.h
    CaptureLib.cpp
)

target_compile_definitions(CaptureLib PRIVATE CAPTURELIB_EXPORTS)

# Link required libraries
target_link_libraries(CaptureLib
    PUBLIC
    windowsapp.lib
    d3d11.lib
    mfplat.lib
    mfuuid.lib
    mfreadwrite.lib
    wmcodecdspuuid.lib
)
