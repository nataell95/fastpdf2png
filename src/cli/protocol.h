// fastpdf2png — IPC protocol structures for pool and raw modes
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace fpdf2png::cli {

/// Job descriptor sent from parent to pool worker via pipe.
struct PipeJob {
    char pdf_path[512];
    char output_pattern[512];  // empty = return raw pixels via pipe
    float dpi;
    int compression;
    int page_start;   // -1 = all pages, >= 0 = start page
    int page_end;     // exclusive end
    int no_aa;        // 1 = disable anti-aliasing
};

/// Result sent from pool worker back to parent via pipe.
struct PipeResult {
    int pages_rendered;
};

/// Header sent before raw pixel data in memory/raw modes.
struct PixelHeader {
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t channels;  // 1 = grayscale, 3 = RGB, 4 = RGBA
};

} // namespace fpdf2png::cli
