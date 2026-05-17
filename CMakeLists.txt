cmake_minimum_required(VERSION 3.16)
project(Vane LANGUAGES CXX OBJCXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 平台检测 ----
if(APPLE)
    set(PLATFORM_MAC ON)
elseif(WIN32)
    set(PLATFORM_WINDOWS ON)
elseif(UNIX)
    set(PLATFORM_LINUX ON)
endif()

# ---- 公共头文件目录 ----
add_subdirectory(src)
add_subdirectory(test)
