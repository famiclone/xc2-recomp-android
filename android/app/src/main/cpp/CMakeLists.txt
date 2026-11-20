cmake_minimum_required(VERSION 3.22.1)
project(xc2recomp LANGUAGES C CXX ASM)

set(XC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
get_filename_component(XC2_ROOT "${XC2_ROOT}" REALPATH)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)

# Target Snapdragon 8 Gen 2 (ARMv9 / ARMv8.2-A + FP16 + DotProd); keep -O2 in Debug too so musttail dispatch and SIMD
# helpers run at full speed while preserving debug symbols.
add_compile_options(
    -O2
    -march=armv8.2-a+fp16+dotprod
    -fno-strict-aliasing
    -fPIC
    -DVK_NO_PROTOTYPES
)

# ---- Third-party dependencies (SDL3, Opus, zstd, libvpx)
# Use prebuilt libraries in XC2_ANDROID_DEPS (or toolchain/deps-android) when present, else FetchContent.
include(FetchContent)

if(NOT DEFINED XC2_ANDROID_DEPS AND EXISTS "${XC2_ROOT}/toolchain/deps-android/include")
    set(XC2_ANDROID_DEPS "${XC2_ROOT}/toolchain/deps-android")
endif()

if(DEFINED XC2_ANDROID_DEPS AND EXISTS "${XC2_ANDROID_DEPS}/include")
    include_directories("${XC2_ANDROID_DEPS}/include")
    link_directories("${XC2_ANDROID_DEPS}/lib")
    add_library(SDL3_prebuilt SHARED IMPORTED)
    set_target_properties(SDL3_prebuilt PROPERTIES IMPORTED_LOCATION "${XC2_ANDROID_DEPS}/lib/libSDL3.so")
    set(XC2_DEP_LIBS SDL3_prebuilt opus vpx zstd)
else()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.8
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.5.2
        GIT_SHALLOW TRUE
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(SDL3 opus zstd)
    set(XC2_DEP_LIBS SDL3::SDL3 opus libzstd_static)
    if(DEFINED LIBVPX_LIBRARY AND EXISTS "${LIBVPX_LIBRARY}")
        include_directories("${LIBVPX_INCLUDE_DIR}")
        list(APPEND XC2_DEP_LIBS "${LIBVPX_LIBRARY}")
    endif()
endif()

# ---- Runtime sources (excluding Win32-only files)
file(GLOB RT_C "${XC2_ROOT}/runtime/*.c")
list(FILTER RT_C EXCLUDE REGEX "_win\\.c$")
file(GLOB RT_CPP
    "${XC2_ROOT}/runtime/*.cpp"
    "${XC2_ROOT}/runtime/ui/*.cpp"
    "${XC2_ROOT}/runtime/third_party/imgui/*.cpp"
    "${XC2_ROOT}/runtime/third_party/imgui/backends/*.cpp"
)
list(FILTER RT_CPP EXCLUDE REGEX "_win\\.cpp$")

set(RT_SOURCES
    ${RT_C}
    ${RT_CPP}
    "${XC2_ROOT}/runtime/platform/common.c"
    "${XC2_ROOT}/runtime/platform/linux.c"
)

# ---- Generated game code vs game-free selftest bring-up
if(NOT DEFINED XC2_ANDROID_VERSION)
    if(EXISTS "${XC2_ROOT}/build/v301-android/modules.c")
        set(XC2_ANDROID_VERSION "v301")
    elseif(EXISTS "${XC2_ROOT}/build/v210-android/modules.c")
        set(XC2_ANDROID_VERSION "v210")
    else()
        set(XC2_ANDROID_VERSION "v301")
    endif()
endif()
set(XC2_ANDROID_BUILD_DIR "${XC2_ROOT}/build/${XC2_ANDROID_VERSION}-android" CACHE PATH "Android game build dir")
set(XC2_ANDROID_GEN_DIR "${XC2_ROOT}/generated/${XC2_ANDROID_VERSION}-android" CACHE PATH "Android generated C dir")

set(XC2_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/xc2_cfg")
file(MAKE_DIRECTORY "${XC2_CONFIG_DIR}")

if(EXISTS "${XC2_ANDROID_BUILD_DIR}/modules.c" AND EXISTS "${XC2_ANDROID_GEN_DIR}/layout.h")
    message(STATUS "XC2Recomp: linking generated game build from ${XC2_ANDROID_BUILD_DIR}")
    file(GLOB_RECURSE GEN_OBJS "${XC2_ANDROID_BUILD_DIR}/obj/main/*.o"
                               "${XC2_ANDROID_BUILD_DIR}/obj/subsdk*/*.o"
                               "${XC2_ANDROID_BUILD_DIR}/obj/sdk/*.o")
    list(APPEND RT_SOURCES "${XC2_ANDROID_BUILD_DIR}/modules.c")
    set(XC2_EXTRA_INC "${XC2_ANDROID_BUILD_DIR}" "${XC2_ANDROID_GEN_DIR}")
else()
    message(STATUS "XC2Recomp: no generated/v301-android found; building selftest bring-up APK")
    set(GEN_OBJS "")
    list(APPEND RT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/selftest_modules.c")
    file(WRITE "${XC2_CONFIG_DIR}/romfs_config.h"
        "#pragma once\n#define ROMFS_MISSING 1\n#define ROMFS_IMAGE_SIZE 0ull\n"
        "#define ROMFS_IMAGE_SHA256 \"\"\n#define ROMFS_INDEX_SHA256 \"\"\n#define ROMFS_FILE_COUNT 0ull\n")
    file(WRITE "${XC2_CONFIG_DIR}/patches.h"
        "#pragma once\n#define RT_PATCH_COUNT 0\n")
    set(XC2_EXTRA_INC "${XC2_CONFIG_DIR}")
endif()

add_library(main SHARED ${RT_SOURCES} ${GEN_OBJS})

target_include_directories(main PRIVATE
    "${XC2_ROOT}/runtime"
    "${XC2_ROOT}/runtime/third_party/imgui"
    "${XC2_ROOT}/toolchain/vma"
    ${XC2_EXTRA_INC}
)

target_link_options(main PRIVATE
    -Wl,-z,noexecstack
    -Wl,-z,max-page-size=16384
)

target_link_libraries(main PRIVATE
    ${XC2_DEP_LIBS}
    android
    log
    dl
    m
)
