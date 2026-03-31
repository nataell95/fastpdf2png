// fastpdf2png — Daemon mode (stdin/stdout command loop)
// SPDX-License-Identifier: MIT

#pragma once

namespace fpdf2png::cli {

/// Run the --daemon stdin/stdout loop.
/// Reads INFO and RENDER commands, responds with results.
int RunDaemon();

} // namespace fpdf2png::cli
