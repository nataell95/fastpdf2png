// fastpdf2png — Raw pixel streaming mode (--raw)
// SPDX-License-Identifier: MIT

#pragma once

namespace fpdf2png::cli {

/// Render all pages and stream PixelHeader + raw RGBA to stdout.
/// Zero disk I/O — intended for in-process consumption by a parent.
int RunRaw(const char* pdf_path, float dpi, bool no_aa);

} // namespace fpdf2png::cli
