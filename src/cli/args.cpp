// fastpdf2png — CLI argument parsing
// SPDX-License-Identifier: MIT

#include "cli/args.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace fpdf2png::cli {

void PrintUsage(const char* prog) {
    std::fprintf(stderr,
        "fastpdf2png - Ultra-fast PDF to PNG converter\n\n"
        "Usage:\n"
        "  %s input.pdf output_%%03d.png [dpi] [workers] [-c level]\n"
        "  %s --pool [dpi] [workers] [-c level]  < job_list\n"
        "  %s --info input.pdf\n"
        "  %s --raw input.pdf [dpi] [--no-aa]\n"
        "  %s --daemon\n\n"
        "Options:\n"
        "  dpi       Resolution (default: 150)\n"
        "  workers   Parallel workers (default: 4)\n"
        "  -c level  -1=raw PPM/PGM, 0=fast, 1=medium, 2=best (default: 2)\n"
        "  --no-aa   Disable anti-aliasing for speed\n\n"
        "Pool mode reads pdf_path\\toutput_pattern lines from stdin.\n",
        prog, prog, prog, prog, prog);
}

bool ParseArgs(int argc, char* argv[], ParsedArgs& out) {
    out = ParsedArgs{};

    if (argc < 2) {
        PrintUsage(argv[0]);
        return false;
    }

    const std::string_view arg1{argv[1]};

    // --info input.pdf
    if (arg1 == "--info" && argc == 3) {
        out.mode = Mode::INFO;
        out.pdf_path = argv[2];
        return true;
    }

    // --raw input.pdf [dpi] [--no-aa]
    if (arg1 == "--raw" && argc >= 3) {
        out.mode = Mode::RAW;
        out.pdf_path = argv[2];
        out.dpi = (argc > 3 && argv[3][0] != '-')
            ? static_cast<float>(std::atof(argv[3])) : 150.0f;
        for (int i = 3; i < argc; ++i)
            if (std::string_view{argv[i]} == "--no-aa") out.no_aa = true;
        return true;
    }

    // --daemon
    if (arg1 == "--daemon" && argc == 2) {
        out.mode = Mode::DAEMON;
        return true;
    }

    // --pool / --batch [dpi] [workers] [-c level] [--no-aa]
    if (arg1 == "--pool" || arg1 == "--batch") {
        out.mode = Mode::POOL;
        out.dpi = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 150.0f;
        out.workers = (argc > 3) ? std::atoi(argv[3]) : 4;
        for (int i = 4; i < argc; ++i) {
            if (std::string_view{argv[i]} == "-c" && i + 1 < argc)
                out.compression = std::clamp(std::atoi(argv[i + 1]), -1, 2);
            if (std::string_view{argv[i]} == "--no-aa")
                out.no_aa = true;
        }
        out.workers = std::clamp(out.workers, 1, kMaxWorkers);
        return true;
    }

#ifdef _WIN32
    // --pool-worker <cmd_handle> <result_handle>
    if (arg1 == "--pool-worker" && argc == 4) {
        out.mode = Mode::POOL_WORKER;
        out.pool_cmd_handle = argv[2];
        out.pool_result_handle = argv[3];
        return true;
    }

    // --worker <pdf> <pattern> <dpi_x10> <compression> <shm_name> [no_aa]
    if (arg1 == "--worker" && (argc == 7 || argc == 8)) {
        out.mode = Mode::WORKER;
        out.pdf_path = argv[2];
        out.pattern = argv[3];
        out.dpi = std::atoi(argv[4]) / 10.0f;
        out.compression = std::atoi(argv[5]);
        out.shm_name = argv[6];
        out.no_aa = (argc == 8) ? std::atoi(argv[7]) != 0 : false;
        return true;
    }
#endif

    // Default render mode: input.pdf output_pattern [dpi] [workers] [-c level] [--no-aa]
    if (argc < 3) {
        PrintUsage(argv[0]);
        return false;
    }

    out.mode = Mode::RENDER;
    out.pdf_path = argv[1];
    out.pattern = argv[2];
    out.dpi = (argc > 3) ? static_cast<float>(std::atof(argv[3])) : 150.0f;
    out.workers = (argc > 4) ? std::atoi(argv[4]) : 4;

    for (int i = 5; i < argc; ++i) {
        if (std::string_view{argv[i]} == "-c" && i + 1 < argc)
            out.compression = std::clamp(std::atoi(argv[i + 1]), -1, 2);
        if (std::string_view{argv[i]} == "--no-aa")
            out.no_aa = true;
    }

    if (out.dpi <= 0 || out.dpi > kMaxDpi) {
        std::fprintf(stderr, "DPI must be 1-%.0f\n", kMaxDpi);
        return false;
    }
    out.workers = std::clamp(out.workers, 1, kMaxWorkers);

    return true;
}

} // namespace fpdf2png::cli
