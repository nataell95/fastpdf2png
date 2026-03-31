// fastpdf2png — CLI argument parsing
// SPDX-License-Identifier: MIT

#pragma once

namespace fpdf2png::cli {

constexpr int kMaxWorkers = 64;
constexpr float kMaxDpi = 2400.0f;

enum class Mode {
    RENDER,
    INFO,
    RAW,
    DAEMON,
    POOL,
    WORKER,        // Windows shared-memory worker
    POOL_WORKER,   // Windows pipe-based pool worker
};

struct ParsedArgs {
    const char* pdf_path = nullptr;
    const char* pattern = nullptr;
    float dpi = 150.0f;
    int workers = 4;
    int compression = 2;
    bool no_aa = false;
    Mode mode = Mode::RENDER;

    // Windows worker-specific fields
    const char* shm_name = nullptr;     // --worker shared memory name
    const char* pool_cmd_handle = nullptr;   // --pool-worker cmd handle
    const char* pool_result_handle = nullptr; // --pool-worker result handle
};

/// Parse command-line arguments into a ParsedArgs struct.
/// Returns true on success, false on validation failure (error printed to stderr).
bool ParseArgs(int argc, char* argv[], ParsedArgs& out);

/// Print usage information to stderr.
void PrintUsage(const char* prog);

} // namespace fpdf2png::cli
