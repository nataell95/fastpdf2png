// fastpdf2png — C API for FFI (Python ctypes, Node ffi-napi, etc.)
// SPDX-License-Identifier: MIT

#pragma once

#include "fastpdf2png/export.h"

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t*  data;
    int32_t   width;
    int32_t   height;
    int32_t   stride;
} fpdf2png_page_c;

/// Initialize the library (call once before any other function).
FPDF2PNG_API void fpdf2png_init(void);

/// Shut down the library and release global resources.
FPDF2PNG_API void fpdf2png_shutdown(void);

/// Return the number of pages in a PDF file, or -1 on error.
FPDF2PNG_API int fpdf2png_page_count(const char* path);

/// Render all pages of a PDF file to RGBA pixel buffers.
/// On success: *out points to a malloc'd array of fpdf2png_page_c, *out_count is the number of pages.
/// Caller must free with fpdf2png_free(*out, *out_count).
/// Returns 0 on success, non-zero error code on failure.
FPDF2PNG_API int fpdf2png_render(const char* path, float dpi, int no_aa,
                                  fpdf2png_page_c** out, int* out_count);

/// Render all pages from an in-memory PDF buffer.
/// Same ownership semantics as fpdf2png_render.
FPDF2PNG_API int fpdf2png_render_mem(const uint8_t* data, size_t size,
                                      float dpi, int no_aa,
                                      fpdf2png_page_c** out, int* out_count);

/// Free page data returned by fpdf2png_render / fpdf2png_render_mem.
/// Frees each page's pixel data and the array itself.
FPDF2PNG_API void fpdf2png_free(fpdf2png_page_c* pages, int count);

#ifdef __cplusplus
} // extern "C"
#endif
