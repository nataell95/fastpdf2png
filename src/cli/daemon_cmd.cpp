// fastpdf2png — Daemon mode (stdin/stdout command loop)
// SPDX-License-Identifier: MIT

#include "cli/daemon_cmd.h"
#include "internal/pdfium_render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "fpdfview.h"

namespace fpdf2png::cli {

namespace {

void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

int SplitTabs(char* line, char** tokens, int max_tokens) {
    int count = 0;
    auto* p = line;
    while (count < max_tokens) {
        tokens[count++] = p;
        auto* tab = std::strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return count;
}

} // anonymous namespace

int RunDaemon() {
    InitPdfium();

    char line[8192];
    while (std::fgets(line, sizeof(line), stdin)) {
        auto len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (std::strncmp(line, "QUIT", 4) == 0) break;

        char* tokens[8];
        const auto ntok = SplitTabs(line, tokens, 8);

        if (ntok >= 2 && std::string_view{tokens[0]} == "INFO") {
            auto* doc = FPDF_LoadDocument(tokens[1], nullptr);
            if (!doc) {
                std::printf("ERROR cannot open\n");
            } else {
                std::printf("OK %d\n", FPDF_GetPageCount(doc));
                FPDF_CloseDocument(doc);
            }
            std::fflush(stdout);
            continue;
        }

        if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
            const auto* pdf = tokens[1];
            const auto* pat = tokens[2];
            const auto dpi = (ntok >= 4)
                ? static_cast<float>(std::atof(tokens[3])) : 150.0f;
            const auto comp = (ntok >= 6) ? std::atoi(tokens[5]) : 2;

            auto* doc = FPDF_LoadDocument(pdf, nullptr);
            if (!doc) {
                std::printf("ERROR cannot open %s\n", pdf);
                std::fflush(stdout);
                continue;
            }
            const auto pages = FPDF_GetPageCount(doc);

            for (int i = 0; i < pages; ++i)
                internal::RenderPageToFile(doc, i, dpi, pat, comp);
            FPDF_CloseDocument(doc);

            std::printf("OK %d\n", pages);
            std::fflush(stdout);
            continue;
        }

        std::printf("ERROR unknown command\n");
        std::fflush(stdout);
    }

    FPDF_DestroyLibrary();
    return 0;
}

} // namespace fpdf2png::cli
