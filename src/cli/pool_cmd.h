// fastpdf2png — Pool mode (pre-forked workers with pipe IPC)
// SPDX-License-Identifier: MIT

#pragma once

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fpdf2png::cli {

#ifndef _WIN32
/// Unix pool mode: pre-fork workers, read jobs from stdin, dispatch via pipes.
int RunPool(int num_workers, float dpi, int compression, bool no_aa);
#endif

#ifdef _WIN32
/// Windows pool mode: CreateProcess workers with anonymous pipe IPC.
int RunPoolWin(int num_workers, float dpi, int compression, bool no_aa);

/// Windows pool worker entry point (invoked via --pool-worker).
int RunPoolWorkerWin(HANDLE cmd_h, HANDLE result_h);
#endif

} // namespace fpdf2png::cli
