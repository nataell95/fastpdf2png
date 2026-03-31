# libdeflate — Fast compression library (MIT)
# .c files compiled as C (NOT C++) to avoid void* cast errors

set(_deflate_srcs
    lib/deflate_compress.c
    lib/adler32.c
    lib/crc32.c
    lib/utils.c
    lib/zlib_compress.c
)

# Architecture-specific CPU feature detection
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    list(APPEND _deflate_srcs lib/arm/cpu_features.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x64)$")
    list(APPEND _deflate_srcs lib/x86/cpu_features.c)
endif()

add_library(libdeflate STATIC ${_deflate_srcs})

target_include_directories(libdeflate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(libdeflate PRIVATE
    SUPPORT_NEAR_OPTIMAL_PARSING=0
)

set_target_properties(libdeflate PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

fp2p_apply_simd(libdeflate)

# Suppress warnings in vendored code
if(MSVC)
    target_compile_options(libdeflate PRIVATE /wd4100 /wd4244)
else()
    target_compile_options(libdeflate PRIVATE -Wno-unused-parameter)
endif()

add_library(libdeflate::libdeflate ALIAS libdeflate)
