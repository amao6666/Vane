# ---- Vane 测试程序 ----
add_executable(VaneTest main.cpp)

target_link_libraries(VaneTest PRIVATE Vane)

# VaneTest 在 Windows 上需要 DXGI 以枚举 NVIDIA 适配器
if(PLATFORM_WINDOWS)
    target_link_libraries(VaneTest PRIVATE dxgi)
endif()

# MP4Writer 独立单元测试
add_executable(Mp4WriterTest test_mp4writer.cpp)
target_link_libraries(Mp4WriterTest PRIVATE Vane)
target_include_directories(Mp4WriterTest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# MSVC 需要显式指定 UTF-8 源文件编码
if(MSVC)
    target_compile_options(VaneTest PRIVATE /utf-8)
    target_compile_options(Mp4WriterTest PRIVATE /utf-8)
endif()

# 当 Vane 为静态库时，测试需定义 VANE_EXPORTS 以跳过 dllimport 修饰
if(NOT VANE_BUILD_SHARED)
    target_compile_definitions(VaneTest PRIVATE VANE_EXPORTS)
    target_compile_definitions(Mp4WriterTest PRIVATE VANE_EXPORTS)
endif()

# 测试用编码延迟注入（与 src/CMakeLists.txt 保持一致，供测试代码条件编译）
if(VANE_TEST_ENCODING_DELAY_MS GREATER 0)
    target_compile_definitions(VaneTest PRIVATE VANE_TEST_ENCODING_DELAY_MS=${VANE_TEST_ENCODING_DELAY_MS})
    target_compile_definitions(Mp4WriterTest PRIVATE VANE_TEST_ENCODING_DELAY_MS=${VANE_TEST_ENCODING_DELAY_MS})
endif()

# 平台宏定义（与 src/CMakeLists.txt 保持一致）
if(PLATFORM_MAC)
    target_compile_definitions(VaneTest PRIVATE PLATFORM_MAC=1)
    target_compile_definitions(Mp4WriterTest PRIVATE PLATFORM_MAC=1)
elseif(PLATFORM_WINDOWS)
    target_compile_definitions(VaneTest PRIVATE PLATFORM_WINDOWS=1)
    target_compile_definitions(Mp4WriterTest PRIVATE PLATFORM_WINDOWS=1)

    # D3D11 最小测试程序
    add_executable(D3D11Test test_d3d11.cpp)
    target_link_libraries(D3D11Test PRIVATE Vane d3d11 dxgi)
    target_include_directories(D3D11Test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(D3D11Test PRIVATE PLATFORM_WINDOWS=1)
    if(MSVC)
        target_compile_options(D3D11Test PRIVATE /utf-8)
    endif()
    if(NOT VANE_BUILD_SHARED)
        target_compile_definitions(D3D11Test PRIVATE VANE_EXPORTS)
    endif()

    # 全编码器验证测试（NVENC / MF H.264 / MF WMV）
    add_executable(AllEncodersTest test_all_encoders.cpp)
    target_link_libraries(AllEncodersTest PRIVATE Vane d3d11 dxgi)
    target_include_directories(AllEncodersTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(AllEncodersTest PRIVATE PLATFORM_WINDOWS=1)
    if(MSVC)
        target_compile_options(AllEncodersTest PRIVATE /utf-8)
    endif()
    if(NOT VANE_BUILD_SHARED)
        target_compile_definitions(AllEncodersTest PRIVATE VANE_EXPORTS)
    endif()
elseif(PLATFORM_LINUX)
    target_compile_definitions(VaneTest PRIVATE PLATFORM_LINUX=1)
    target_compile_definitions(Mp4WriterTest PRIVATE PLATFORM_LINUX=1)
endif()
