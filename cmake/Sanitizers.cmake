# Sanitizers.cmake — AddressSanitizer and UBSan configuration

option(FP2P_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(FP2P_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(FP2P_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer enabled")
endif()

if(FP2P_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
endif()
