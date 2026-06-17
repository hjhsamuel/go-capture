cmake_minimum_required(VERSION 3.10)
project(CaptureTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable C++/WinRT
add_compile_options(/await)

include_directories(../CaptureLib)

# 查找 CaptureLib 生成的库文件
link_directories(${CMAKE_BINARY_DIR}/Release)

add_executable(CaptureTest main.cpp)

# 链接 CaptureLib 及其依赖项
target_link_libraries(CaptureTest 
    CaptureLib 
    windowsapp 
    mfplat 
    mfuuid 
    mfreadwrite
)
