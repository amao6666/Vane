# ---- 构建选项 ----
option(VANE_BUILD_SHARED "Build as shared library (DLL/dylib/so)" ON)
option(VANE_TEST_ENCODING_DELAY_MS "Inject encoding delay (ms) for testing (0 = disabled)" 0)

# ---- Vane 编码器库 ----
if(VANE_BUILD_SHARED)
    add_library(Vane SHARED)
else()
    add_library(Vane STATIC)
endif()
# 构建库时始终定义 VANE_EXPORTS（静态库时 __declspec(dllexport) 无害）
target_compile_definitions(Vane PRIVATE VANE_EXPORTS)

# MSVC 需要显式指定 UTF-8 源文件编码（避免中文注释/字符串的 C2001/C4819 错误）
if(MSVC)
    target_compile_options(Vane PRIVATE /utf-8)
endif()

target_include_directories(Vane
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
)

# 测试用编码延迟注入
if(VANE_TEST_ENCODING_DELAY_MS GREATER 0)
    target_compile_definitions(Vane PRIVATE VANE_TEST_ENCODING_DELAY_MS=${VANE_TEST_ENCODING_DELAY_MS})
endif()

# 调试计时
option(VANE_DEBUG_TIMING "Enable encode pipeline timing diagnostics" OFF)
if(VANE_DEBUG_TIMING)
    target_compile_definitions(Vane PRIVATE VANE_DEBUG_TIMING=1)
endif()

# 无 DRM 环境下的模拟编码模式（仅用于开发/测试管线逻辑）
option(VANE_FAKE_VAAPI "Enable fake VA-API mode when DRM is unavailable (dev/test only)" OFF)
if(VANE_FAKE_VAAPI)
    target_compile_definitions(Vane PRIVATE VANE_FAKE_VAAPI=1)
endif()

# 跨平台核心（始终编译）
target_sources(Vane PRIVATE
    VaneAPI.cpp
    AsyncPipeline.cpp
    core/MP4Writer.cpp
    core/MP4Writer.h
    core/ColorSpaceConverter.cpp
    core/ColorSpaceConverter.h
    core/D3D11Converter.cpp
    core/D3D11Converter.h
)

# ---- 平台分支 ----
if(PLATFORM_MAC)
    target_compile_definitions(Vane PRIVATE PLATFORM_MAC=1)

    find_library(VIDEOTOOLBOX_FWK VideoToolbox REQUIRED)
    find_library(COREMEDIA_FWK   CoreMedia   REQUIRED)
    find_library(COREVIDEO_FWK   CoreVideo   REQUIRED)
    find_library(AVFOUNDATION_FWK AVFoundation REQUIRED)
    find_library(FOUNDATION_FWK   Foundation  REQUIRED)

    target_sources(Vane PRIVATE FVTEncoder.mm)

    target_compile_options(Vane PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )

    target_link_libraries(Vane PUBLIC
        ${VIDEOTOOLBOX_FWK}
        ${COREMEDIA_FWK}
        ${COREVIDEO_FWK}
        ${AVFOUNDATION_FWK}
        ${FOUNDATION_FWK}
    )
elseif(PLATFORM_WINDOWS)
    target_compile_definitions(Vane PRIVATE PLATFORM_WINDOWS=1)

    # 自动下载 NVIDIA NVENC 头文件（FFmpeg 维护的 MIT 许可镜像，~200KB）
    set(NVENC_HEADER_DIR ${CMAKE_BINARY_DIR}/nv-codec-headers/include/ffnvcodec)
    if(NOT EXISTS ${NVENC_HEADER_DIR}/nvEncodeAPI.h)
        file(MAKE_DIRECTORY ${NVENC_HEADER_DIR})
        file(DOWNLOAD
            https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/master/include/ffnvcodec/nvEncodeAPI.h
            ${NVENC_HEADER_DIR}/nvEncodeAPI.h
            STATUS DOWNLOAD_STATUS
        )
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        if(NOT STATUS_CODE EQUAL 0)
            list(GET DOWNLOAD_STATUS 1 STATUS_MSG)
            message(FATAL_ERROR "无法下载 nvEncodeAPI.h: ${STATUS_MSG}\n"
                "请手动下载: git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers.git\n"
                "并放到 ${NVENC_HEADER_DIR}/")
        endif()
    endif()

    target_include_directories(Vane PRIVATE
        ${CMAKE_BINARY_DIR}/nv-codec-headers/include/ffnvcodec
    )

    target_sources(Vane PRIVATE
        # 新架构：FWindowsEncoder 门面 + 三路编码器
        windows/FWindowsEncoder.cpp
        windows/FWindowsEncoder.h
        windows/FNvencEncoder.cpp
        windows/FNvencEncoder.h
        windows/FAmfEncoder.cpp
        windows/FAmfEncoder.h
        windows/FMFEncoderNew.cpp
        windows/FMFEncoderNew.h
        windows/MFUtils.h
        # 保留旧版 FMFEncoder（不编译，仅作参考）
        # FMFEncoder.cpp
    )

    target_link_libraries(Vane PUBLIC
        mfplat
        mfreadwrite
        mfuuid
        ole32
        d3d11
    )
elseif(PLATFORM_LINUX)
    target_compile_definitions(Vane PRIVATE PLATFORM_LINUX=1)

    find_library(LIBVA_LIB va REQUIRED)
    find_library(LIBVA_DRM_LIB va-drm REQUIRED)

    target_sources(Vane PRIVATE
        FVAEncoder.cpp
        MP4Muxer.cpp
    )

    target_link_libraries(Vane PUBLIC ${LIBVA_LIB} ${LIBVA_DRM_LIB})
endif()
