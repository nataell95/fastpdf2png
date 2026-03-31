// fastpdf2png — CLI entry point
// SPDX-License-Identifier: MIT
//
// Thin dispatcher: parses args, initializes PDFium where needed,
// delegates to the appropriate command module.

#include "cli/args.h"
#include "cli/render_cmd.h"
#include "cli/daemon_cmd.h"
#include "cli/pool_cmd.h"
#include "cli/raw_cmd.h"

#include <cstdio>

#include "fpdfview.h"

namespace {

void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    using namespace fpdf2png::cli;

    ParsedArgs args;
    if (!ParseArgs(argc, argv, args))
        return 1;

    switch (args.mode) {
    case Mode::INFO: {
        InitPdfium();
        auto* doc = FPDF_LoadDocument(args.pdf_path, nullptr);
        if (!doc) {
            std::fprintf(stderr, "Failed to open: %s\n", args.pdf_path);
            FPDF_DestroyLibrary();
            return 1;
        }
        std::printf("%d\n", FPDF_GetPageCount(doc));
        FPDF_CloseDocument(doc);
        FPDF_DestroyLibrary();
        return 0;
    }

    case Mode::RAW: {
        InitPdfium();
        const auto rc = RunRaw(args.pdf_path, args.dpi, args.no_aa);
        FPDF_DestroyLibrary();
        return rc;
    }

    case Mode::DAEMON:
        return RunDaemon();

    case Mode::POOL:
#ifndef _WIN32
        return RunPool(args.workers, args.dpi, args.compression, args.no_aa);
#else
        return RunPoolWin(args.workers, args.dpi, args.compression, args.no_aa);
#endif

#ifdef _WIN32
    case Mode::POOL_WORKER: {
        auto cmd_h = reinterpret_cast<HANDLE>(
            std::strtoull(args.pool_cmd_handle, nullptr, 10));
        auto result_h = reinterpret_cast<HANDLE>(
            std::strtoull(args.pool_result_handle, nullptr, 10));
        return RunPoolWorkerWin(cmd_h, result_h);
    }

    case Mode::WORKER:
        return RunWindowsWorker(args.pdf_path, args.dpi, args.pattern,
                                args.compression, args.shm_name, args.no_aa);
#else
    case Mode::POOL_WORKER:
    case Mode::WORKER:
        std::fprintf(stderr, "Worker modes are Windows-only\n");
        return 1;
#endif

    case Mode::RENDER:
        return RunRender(args);
    }

    return 1;
}
