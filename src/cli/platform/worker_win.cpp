// fastpdf2png — Windows shared-memory worker for multi-process rendering
// SPDX-License-Identifier: MIT
//
// Only compiled on Windows. Declarations live in render_cmd.h.

#ifdef _WIN32

#include "cli/render_cmd.h"
#include "internal/pdfium_render.h"

#include <cstdio>

#define NOMINMAX
#include <windows.h>

#include "fpdfview.h"

namespace fpdf2png::cli {

namespace {

// Mirror of the SharedState defined in render_cmd.cpp for Windows.
struct SharedState {
    volatile LONG next_page;
    char pad1[60];
    volatile LONG completed_pages;
    char pad2[60];
    int total_pages;
};

inline int ClaimNextPage(SharedState* s)  { return InterlockedExchangeAdd(&s->next_page, 1); }
inline void MarkCompleted(SharedState* s) { InterlockedExchangeAdd(&s->completed_pages, 1); }

void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

} // anonymous namespace

int RunWindowsWorker(const char* pdf_path, float dpi, const char* pattern,
                     int compression, const char* shm_name, bool no_aa) {
    InitPdfium();

    auto hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    if (!hMap) {
        std::fprintf(stderr, "Worker: OpenFileMapping failed (%lu)\n", GetLastError());
        FPDF_DestroyLibrary();
        return 1;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(hMap); FPDF_DestroyLibrary(); return 1; }

    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) {
        UnmapViewOfFile(shared);
        CloseHandle(hMap);
        FPDF_DestroyLibrary();
        return 1;
    }

    while (true) {
        const auto page = ClaimNextPage(shared);
        if (page >= shared->total_pages) break;
        if (internal::RenderPageToFile(doc, page, dpi, pattern, compression, no_aa))
            MarkCompleted(shared);
    }

    FPDF_CloseDocument(doc);
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    FPDF_DestroyLibrary();
    return 0;
}

} // namespace fpdf2png::cli

#endif // _WIN32
