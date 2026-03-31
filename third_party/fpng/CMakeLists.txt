# fpng — Fast PNG encoder by Richard Geldreich (Unlicense)
add_library(fpng STATIC fpng.cpp)
target_include_directories(fpng PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(fpng PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
fp2p_apply_simd(fpng)
