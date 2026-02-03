cmake_minimum_required(VERSION 3.10)
project(DirettaLocalPlayer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define paths
# Auto-detect Diretta SDK path with fallback

set(DIRETTA_SDK_ROOT "")

# Check if DIRETTA_SDK_PATH is set in environment
if(DEFINED ENV{DIRETTA_SDK_PATH})
    set(DIRETTA_SDK_ROOT "$ENV{DIRETTA_SDK_PATH}")
    message(STATUS "Using Diretta SDK from environment variable: ${DIRETTA_SDK_ROOT}")
endif()

# If not found in environment, search in common locations
if(DIRETTA_SDK_ROOT STREQUAL "")
    set(SDK_SEARCH_PATHS 
        "${CMAKE_SOURCE_DIR}/../DirettaHostSDK_148"
        "${CMAKE_SOURCE_DIR}/DirettaHostSDK_148"
        "$ENV{HOME}/DirettaHostSDK_148"
        "/opt/DirettaHostSDK_148"
        "$ENV{HOME}/audio/DirettaHostSDK_148"
        "/usr/local/DirettaHostSDK_148"
    )
    
    foreach(path IN LISTS SDK_SEARCH_PATHS)
        if(EXISTS "${path}")
            set(DIRETTA_SDK_ROOT "${path}")
            message(STATUS "Auto-detected Diretta SDK: ${DIRETTA_SDK_ROOT}")
            break()
        endif()
    endforeach()
endif()

# If still not found, use default path as fallback
if(DIRETTA_SDK_ROOT STREQUAL "")
    set(DIRETTA_SDK_ROOT "${CMAKE_SOURCE_DIR}/../DirettaHostSDK_148")
    message(WARNING "Diretta SDK not found in standard locations. Using default path: ${DIRETTA_SDK_ROOT}")
endif()

set(DIRETTA_INCLUDE_DIR "${DIRETTA_SDK_ROOT}/Host")
set(DIRETTA_LIB_DIR "${DIRETTA_SDK_ROOT}/lib")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${DIRETTA_INCLUDE_DIR})

# Temporarily disabled custom FFmpeg (lacks DFF/IFF support)
# set(CUSTOM_FFMPEG_PATH "${CMAKE_SOURCE_DIR}/ffmpeg_custom")
# if(EXISTS "${CUSTOM_FFMPEG_PATH}")
#     message(STATUS "Using custom FFmpeg build at ${CUSTOM_FFMPEG_PATH}")
#     set(ENV{PKG_CONFIG_PATH} "${CUSTOM_FFMPEG_PATH}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
#     include_directories(${CUSTOM_FFMPEG_PATH}/include)
#     link_directories(${CUSTOM_FFMPEG_PATH}/lib)
# else()
#     message(WARNING "Custom FFmpeg not found at ${CUSTOM_FFMPEG_PATH}, falling back to system")
# endif()
message(STATUS "Using system FFmpeg (for DFF/IFF support)")

# Find FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswresample)
include_directories(${FFMPEG_INCLUDE_DIRS})
link_directories(${FFMPEG_LIBRARY_DIRS})


# Architecture Detection and Library Selection
# Supports: x64 (v2/v3/v4/zen4), aarch64 (standard/k16 for RPi5), riscv64

# Check for NOLOG option (to use -nolog library variants)
option(NOLOG "Use -nolog library variants" OFF)
if(NOLOG)
    set(NOLOG_SUFFIX "-nolog")
    message(STATUS "Using -nolog library variants")
else()
    set(NOLOG_SUFFIX "")
endif()

if(NOT DEFINED DIRETTA_ARCH)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        message(STATUS "Detected architecture: x86_64")
        
        # Enhanced CPU feature detection for x86_64
        execute_process(COMMAND grep "avx2" /proc/cpuinfo OUTPUT_VARIABLE AVX2_INFO)
        execute_process(COMMAND grep "avx512" /proc/cpuinfo OUTPUT_VARIABLE AVX512_INFO)
        execute_process(COMMAND lscpu OUTPUT_VARIABLE LSCUP_INFO)
        string(TOLOWER "${LSCUP_INFO}" LSCUP_INFO_LOWER)
        
        if("${LSCUP_INFO_LOWER}" MATCHES "amd.*zen 4")
            message(STATUS "AMD Zen 4 detected! Using optimized SDK zen4 variant.")
            set(DIRETTA_ARCH "x64-linux-15zen4")
        elseif(AVX512_INFO)
            message(STATUS "AVX512 detected! Using optimized SDK v4.")
            set(DIRETTA_ARCH "x64-linux-15v4")
        elseif(AVX2_INFO)
            message(STATUS "AVX2 detected! Using optimized SDK v3.")
            set(DIRETTA_ARCH "x64-linux-15v3")
        else()
            message(STATUS "Basic x64 detected. Using standard SDK v2.")
            set(DIRETTA_ARCH "x64-linux-15v2")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        message(STATUS "Detected architecture: aarch64")
        
        # Check for Raspberry Pi 5 and 16KB pages
        execute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE)
        
        # Check device model for Raspberry Pi 5 detection
        if(EXISTS "/proc/device-tree/model")
            file(READ "/proc/device-tree/model" DEVICE_MODEL)
            string(TOLOWER "${DEVICE_MODEL}" DEVICE_MODEL_LOWER)
            if("${DEVICE_MODEL_LOWER}" MATCHES "raspberry pi 5")
                message(STATUS "Raspberry Pi 5 detected! Using 16KB page variant.")
                set(DIRETTA_ARCH "aarch64-linux-15k16")
            else()
                # Check page size as fallback
                if(PAGE_SIZE STREQUAL "16384")
                    message(STATUS "16KB page size detected! Using k16 variant.")
                    set(DIRETTA_ARCH "aarch64-linux-15k16")
                else()
                    message(STATUS "Standard aarch64 variant.")
                    set(DIRETTA_ARCH "aarch64-linux-15")
                endif()
            endif()
        else()
            # Fallback to page size check
            if(PAGE_SIZE STREQUAL "16384")
                message(STATUS "16KB page size detected! Using k16 variant.")
                set(DIRETTA_ARCH "aarch64-linux-15k16")
            else()
                message(STATUS "Standard aarch64 variant.")
                set(DIRETTA_ARCH "aarch64-linux-15")
            endif()
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
        message(STATUS "Detected architecture: riscv64")
        set(DIRETTA_ARCH "riscv64-linux-15")
    else()
        message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}. Defaulting to x64-linux-15v2")
        set(DIRETTA_ARCH "x64-linux-15v2")
    endif()
else()
    message(STATUS "Using user-specified Diretta architecture: ${DIRETTA_ARCH}")
endif()

message(STATUS "Selecting Diretta SDK variant: ${DIRETTA_ARCH}")

set(DIRETTA_ACQUA_LIB "ACQUA_${DIRETTA_ARCH}${NOLOG_SUFFIX}")
set(DIRETTA_HOST_LIB "DirettaHost_${DIRETTA_ARCH}${NOLOG_SUFFIX}")

# Verify SDK libraries exist
message(STATUS "Checking for Diretta SDK libraries:")
message(STATUS "  Looking for ${DIRETTA_LIB_DIR}/lib${DIRETTA_HOST_LIB}.a")
message(STATUS "  Looking for ${DIRETTA_LIB_DIR}/lib${DIRETTA_ACQUA_LIB}.a")

if(NOT EXISTS "${DIRETTA_LIB_DIR}/lib${DIRETTA_HOST_LIB}.a")
    message(FATAL_ERROR "Required library not found: ${DIRETTA_LIB_DIR}/lib${DIRETTA_HOST_LIB}.a")
endif()

if(NOT EXISTS "${DIRETTA_LIB_DIR}/lib${DIRETTA_ACQUA_LIB}.a")
    message(FATAL_ERROR "Required library not found: ${DIRETTA_LIB_DIR}/lib${DIRETTA_ACQUA_LIB}.a")
endif()

# Find Threads
find_package(Threads REQUIRED)

# Additional dependencies for extended format support (matching logic from static build docs)
# These libraries enable FFmpeg to support specific containers/protocols (HTTPS, compressed headers, etc.)
find_package(OpenSSL)
find_library(LIBLZMA lzma)
find_library(LIBBZ2 bz2)
find_package(ZLIB)

# Source files
file(GLOB SOURCES "src/*.cpp")

# Executable
add_executable(DirettaLocalPlayer ${SOURCES})

# Link libraries
target_link_libraries(DirettaLocalPlayer
    PRIVATE
    ${DIRETTA_LIB_DIR}/lib${DIRETTA_HOST_LIB}.a
    ${DIRETTA_LIB_DIR}/lib${DIRETTA_ACQUA_LIB}.a
    ${FFMPEG_LIBRARIES}
    Threads::Threads
    dl
)

# Optional libraries for format support
if(OPENSSL_FOUND)
    target_link_libraries(DirettaLocalPlayer PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(LIBLZMA)
    target_link_libraries(DirettaLocalPlayer PRIVATE ${LIBLZMA})
endif()
if(LIBBZ2)
    target_link_libraries(DirettaLocalPlayer PRIVATE ${LIBBZ2})
endif()
if(ZLIB_FOUND)
    target_link_libraries(DirettaLocalPlayer PRIVATE ZLIB::ZLIB)
endif()

# RISC-V and ARM often require libatomic explicitly
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_link_libraries(DirettaLocalPlayer PRIVATE atomic)
endif()

# Compile definitions and optimizations
target_compile_definitions(DirettaLocalPlayer PRIVATE _USE_MATH_DEFINES)

# Optimization flags (根据架构选择合适的指令集)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(DIRETTA_ARCH MATCHES "x64-linux-15v2")
        # 基础x86-64架构，不使用AVX2
        target_compile_options(DirettaLocalPlayer PRIVATE -O3 -march=x86-64 -Wall -Wextra -Wno-implicit-fallthrough)
    elseif(DIRETTA_ARCH MATCHES "x64-linux-15v3")
        # 支持AVX2的架构
        target_compile_options(DirettaLocalPlayer PRIVATE -O3 -march=haswell -Wall -Wextra -Wno-implicit-fallthrough)
    elseif(DIRETTA_ARCH MATCHES "x64-linux-15v4")
        # 支持AVX512的架构
        target_compile_options(DirettaLocalPlayer PRIVATE -O3 -march=skylake-avx512 -Wall -Wextra -Wno-implicit-fallthrough)
    elseif(DIRETTA_ARCH MATCHES "x64-linux-15zen4")
        # AMD Zen 4架构
        target_compile_options(DirettaLocalPlayer PRIVATE -O3 -march=znver4 -Wall -Wextra -Wno-implicit-fallthrough)
    else()
        # 其他架构使用默认优化
        target_compile_options(DirettaLocalPlayer PRIVATE -O3 -Wall -Wextra -Wno-implicit-fallthrough)
    endif()
endif()
