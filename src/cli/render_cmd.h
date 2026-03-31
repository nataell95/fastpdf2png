// fastpdf2png — Single-file render command
// SPDX-License-Identifier: MIT

#pragma once

#include "cli/args.h"

namespace fpdf2png::cli {

/// Run single-file rendering (single-process or multi-process via fork/CreateProcess).
int RunRender(const ParsedArgs& args);

#ifdef _WIN32
/// Windows shared-memory worker entry point (invoked via --worker).
int RunWindowsWorker(const char* pdf_path, float dpi, const char* pattern,
                     int compression, const char* shm_name, bool no_aa);
#endif

} // namespace fpdf2png::cli
