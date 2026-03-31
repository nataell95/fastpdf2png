# FindPDFium.cmake — Locate PDFium installation
#
# Sets:
#   PDFium_FOUND
#   PDFium_INCLUDE_DIRS
#   PDFium_LIBRARIES
#   PDFium_LIBRARY_DIR
# Creates imported target: PDFium::PDFium

# Allow user override via -DPDFium_DIR=...
if(NOT PDFium_DIR)
    set(PDFium_DIR "${CMAKE_SOURCE_DIR}/pdfium")
endif()

find_path(PDFium_INCLUDE_DIR
    NAMES fpdfview.h
    PATHS "${PDFium_DIR}/include"
    NO_DEFAULT_PATH
)

# Try config-based find first (skip on Windows — config sets DLL path not import lib)
if(EXISTS "${PDFium_DIR}/PDFiumConfig.cmake" AND NOT WIN32)
    include("${PDFium_DIR}/PDFiumConfig.cmake")
endif()

# Find the library
if(WIN32)
    # On Windows, link against the import library (.dll.lib), NOT the DLL itself.
    # Use find_file (not find_library) to avoid CMake finding pdfium.dll by mistake.
    find_file(PDFium_LIBRARY
        NAMES pdfium.dll.lib pdfium.lib
        PATHS "${PDFium_DIR}/lib"
        NO_DEFAULT_PATH
    )
    find_file(PDFium_DLL
        NAMES pdfium.dll
        PATHS "${PDFium_DIR}/bin" "${PDFium_DIR}/lib"
        NO_DEFAULT_PATH
    )
elseif(APPLE)
    find_library(PDFium_LIBRARY
        NAMES pdfium libpdfium.dylib
        PATHS "${PDFium_DIR}/lib"
        NO_DEFAULT_PATH
    )
else()
    find_library(PDFium_LIBRARY
        NAMES pdfium libpdfium.so libpdfium.a
        PATHS "${PDFium_DIR}/lib"
        NO_DEFAULT_PATH
    )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PDFium
    REQUIRED_VARS PDFium_LIBRARY PDFium_INCLUDE_DIR
)

if(PDFium_FOUND AND NOT TARGET PDFium::PDFium)
    add_library(PDFium::PDFium SHARED IMPORTED)
    set_target_properties(PDFium::PDFium PROPERTIES
        IMPORTED_LOCATION "${PDFium_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PDFium_INCLUDE_DIR}"
    )
    if(WIN32 AND PDFium_DLL)
        set_target_properties(PDFium::PDFium PROPERTIES
            IMPORTED_IMPLIB "${PDFium_LIBRARY}"
            IMPORTED_LOCATION "${PDFium_DLL}"
        )
    endif()
    get_filename_component(PDFium_LIBRARY_DIR "${PDFium_LIBRARY}" DIRECTORY)
endif()

mark_as_advanced(PDFium_INCLUDE_DIR PDFium_LIBRARY PDFium_DLL)
