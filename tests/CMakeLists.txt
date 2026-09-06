include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Unit tests
add_executable(test_png_writer unit/test_png_writer.cpp)
target_link_libraries(test_png_writer PRIVATE GTest::gtest_main fastpdf2png_lib)
target_include_directories(test_png_writer PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/stb)
fp2p_apply_simd(test_png_writer)

# Integration tests (need a PDF fixture)
add_executable(test_roundtrip integration/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE GTest::gtest_main fastpdf2png_lib PDFium::PDFium)
target_include_directories(test_roundtrip PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/stb)

add_executable(test_production integration/test_production.cpp)
target_link_libraries(test_production PRIVATE GTest::gtest_main fastpdf2png_lib)
target_include_directories(test_production PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/stb)

# Set RPATH so test binaries find libpdfium and libfastpdf2png
if(APPLE)
    set_target_properties(test_png_writer test_roundtrip test_production PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR};${PDFium_LIBRARY_DIR}"
    )
elseif(UNIX)
    set_target_properties(test_png_writer test_roundtrip test_production PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}:${PDFium_LIBRARY_DIR}"
    )
endif()

# Copy PDFium next to test binaries for discovery
add_custom_command(TARGET test_png_writer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PDFium_LIBRARY}" "${CMAKE_CURRENT_BINARY_DIR}/"
    COMMENT "Copying PDFium for tests"
)

include(GoogleTest)
gtest_discover_tests(test_png_writer
    DISCOVERY_TIMEOUT 10
    PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}:${PDFium_LIBRARY_DIR}"
)

# Only discover integration tests if fixture exists
if(EXISTS "${PROJECT_SOURCE_DIR}/bench_sample.pdf")
    gtest_discover_tests(test_roundtrip PROPERTIES ENVIRONMENT "TEST_PDF=${PROJECT_SOURCE_DIR}/bench_sample.pdf")
    gtest_discover_tests(test_production PROPERTIES ENVIRONMENT "TEST_PDF=${PROJECT_SOURCE_DIR}/bench_sample.pdf")
endif()

# Python tests
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME python_tests
        COMMAND ${Python3_EXECUTABLE} -m pytest ${PROJECT_SOURCE_DIR}/tests/python/ -v
    )
    set_tests_properties(python_tests PROPERTIES
        ENVIRONMENT "FASTPDF2PNG_BIN=$<TARGET_FILE:fastpdf2png_bin>"
        TIMEOUT 120
    )
endif()
