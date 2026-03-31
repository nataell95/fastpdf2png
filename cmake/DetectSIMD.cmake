# DetectSIMD.cmake — Detect and configure SIMD flags per-platform
# Sets FP2P_SIMD_C_FLAGS and FP2P_SIMD_CXX_FLAGS

include(CheckCXXCompilerFlag)
include(CheckCCompilerFlag)

set(FP2P_SIMD_C_FLAGS "")
set(FP2P_SIMD_CXX_FLAGS "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x64)$")
    # Try x86-64-v3 first (covers AVX2 + SSE4.1 + SSSE3 + PCLMUL)
    check_cxx_compiler_flag("-march=x86-64-v3" HAS_X86_64_V3)
    if(HAS_X86_64_V3)
        list(APPEND FP2P_SIMD_CXX_FLAGS -march=x86-64-v3 -mavx2 -msse4.1 -mssse3 -mpclmul)
        list(APPEND FP2P_SIMD_C_FLAGS -march=x86-64-v3 -mavx2 -msse4.1 -mssse3 -mpclmul)
    else()
        check_cxx_compiler_flag("-mavx2" HAS_AVX2)
        if(HAS_AVX2)
            list(APPEND FP2P_SIMD_CXX_FLAGS -mavx2 -msse4.1 -mssse3 -mpclmul)
            list(APPEND FP2P_SIMD_C_FLAGS -mavx2 -msse4.1 -mssse3 -mpclmul)
        endif()
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # ARM: use native tuning for local builds, generic for CI
    if(FP2P_NATIVE_ARCH)
        check_cxx_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
        if(HAS_MCPU_NATIVE)
            list(APPEND FP2P_SIMD_CXX_FLAGS -mcpu=native)
            list(APPEND FP2P_SIMD_C_FLAGS -mcpu=native)
        endif()
    endif()
endif()

# Helper function to apply SIMD flags to a target
function(fp2p_apply_simd TARGET)
    if(FP2P_SIMD_CXX_FLAGS OR FP2P_SIMD_C_FLAGS)
        target_compile_options(${TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:${FP2P_SIMD_CXX_FLAGS}>
            $<$<COMPILE_LANGUAGE:C>:${FP2P_SIMD_C_FLAGS}>
        )
    endif()
endfunction()
