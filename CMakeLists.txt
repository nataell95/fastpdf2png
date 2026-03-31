cmake_minimum_required(VERSION 3.25)

# Read version from VERSION file
file(READ "${CMAKE_SOURCE_DIR}/VERSION" FP2P_VERSION)
string(STRIP "${FP2P_VERSION}" FP2P_VERSION)

project(fastpdf2png
    VERSION ${FP2P_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "Ultra-fast PDF to PNG converter"
)

# ── Options ──────────────────────────────────────────────────────────
option(FP2P_BUILD_CLI "Build the CLI executable" ON)
option(FP2P_BUILD_SHARED_LIB "Build shared library" ON)
option(FP2P_BUILD_TESTS "Build tests" OFF)
option(FP2P_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(FP2P_NATIVE_ARCH "Use -mcpu=native on ARM" ON)

# ── Global settings ──────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── CMake modules ────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(DetectSIMD)
include(Sanitizers)

# ── Find PDFium ──────────────────────────────────────────────────────
# Download if not present
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/pdfium/include/fpdfview.h")
    message(STATUS "PDFium not found, running get_pdfium.sh...")
    execute_process(
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/get_pdfium.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _pdfium_result
    )
    if(NOT _pdfium_result EQUAL 0)
        message(FATAL_ERROR "Failed to download PDFium. Run scripts/get_pdfium.sh manually.")
    endif()
endif()

find_package(PDFium REQUIRED)

# ── Third-party libraries ────────────────────────────────────────────
add_subdirectory(third_party/fpng)
add_subdirectory(third_party/libdeflate)

# ── Core library (libfastpdf2png) ────────────────────────────────────
set(_lib_sources
    src/lib/engine.cpp
    src/lib/pool.cpp
    src/lib/c_api.cpp
    src/internal/pdfium_render.cpp
    src/internal/file_io.cpp
    src/internal/shared_memory.cpp
    src/png/png_writer.cpp
)

if(FP2P_BUILD_SHARED_LIB)
    add_library(fastpdf2png_lib SHARED ${_lib_sources})
    target_compile_definitions(fastpdf2png_lib PRIVATE FASTPDF2PNG_BUILD_DLL)
else()
    add_library(fastpdf2png_lib STATIC ${_lib_sources})
endif()

target_include_directories(fastpdf2png_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(fastpdf2png_lib
    PRIVATE PDFium::PDFium fpng libdeflate::libdeflate
)

# Platform-specific link flags
if(APPLE)
    target_link_libraries(fastpdf2png_lib PRIVATE
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreText"
        z
    )
elseif(UNIX)
    target_link_libraries(fastpdf2png_lib PRIVATE pthread dl z)
endif()

fp2p_apply_simd(fastpdf2png_lib)

# Compiler warnings
if(MSVC)
    target_compile_options(fastpdf2png_lib PRIVATE /W4 /wd4100)
else()
    target_compile_options(fastpdf2png_lib PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    )
endif()

set_target_properties(fastpdf2png_lib PROPERTIES
    OUTPUT_NAME fastpdf2png
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

add_library(fastpdf2png::fastpdf2png ALIAS fastpdf2png_lib)

# ── CLI executable ───────────────────────────────────────────────────
if(FP2P_BUILD_CLI)
    set(_cli_sources
        src/cli/main.cpp
        src/cli/args.cpp
        src/cli/render_cmd.cpp
        src/cli/daemon_cmd.cpp
        src/cli/pool_cmd.cpp
        src/cli/raw_cmd.cpp
    )
    if(WIN32)
        list(APPEND _cli_sources src/cli/platform/worker_win.cpp)
    endif()

    # On Windows, internal symbols are not exported from the DLL, so the CLI
    # must compile internal sources directly rather than linking through the shared lib.
    if(WIN32)
        list(APPEND _cli_sources
            src/internal/pdfium_render.cpp
            src/internal/file_io.cpp
            src/internal/shared_memory.cpp
            src/png/png_writer.cpp
        )
    endif()

    add_executable(fastpdf2png_bin ${_cli_sources})
    target_link_libraries(fastpdf2png_bin PRIVATE
        fastpdf2png_lib PDFium::PDFium
    )
    if(WIN32)
        target_link_libraries(fastpdf2png_bin PRIVATE fpng libdeflate::libdeflate)
    endif()
    target_include_directories(fastpdf2png_bin PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
    )
    fp2p_apply_simd(fastpdf2png_bin)

    set_target_properties(fastpdf2png_bin PROPERTIES OUTPUT_NAME fastpdf2png)

    if(MSVC)
        target_compile_options(fastpdf2png_bin PRIVATE /W4 /wd4100)
    else()
        target_compile_options(fastpdf2png_bin PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter
        )
    endif()

    # RPATH for finding PDFium at runtime
    if(APPLE)
        set_target_properties(fastpdf2png_bin PROPERTIES
            INSTALL_RPATH "@executable_path"
            BUILD_RPATH "@executable_path"
        )
    elseif(UNIX)
        set_target_properties(fastpdf2png_bin PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_RPATH "$ORIGIN"
        )
    endif()

    # Copy PDFium shared library next to binary
    if(PDFium_LIBRARY AND NOT PDFium_LIBRARY MATCHES "\\.a$")
        add_custom_command(TARGET fastpdf2png_bin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_LIBRARY}"
                "$<TARGET_FILE_DIR:fastpdf2png_bin>"
            COMMENT "Copying PDFium library to build directory"
        )
        if(APPLE)
            add_custom_command(TARGET fastpdf2png_bin POST_BUILD
                COMMAND install_name_tool -change
                    "./libpdfium.dylib"
                    "@executable_path/libpdfium.dylib"
                    "$<TARGET_FILE:fastpdf2png_bin>"
                COMMENT "Fixing PDFium rpath"
            )
        endif()
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────
if(FP2P_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────
if(FP2P_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ── Install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS fastpdf2png_lib
    EXPORT fastpdf2pngTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/fastpdf2png
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(FP2P_BUILD_CLI)
    install(TARGETS fastpdf2png_bin
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
