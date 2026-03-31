// fastpdf2png — Raw pixel streaming mode (--raw)
// SPDX-License-Identifier: MIT

#include "cli/raw_cmd.h"
#include "cli/protocol.h"
#include "internal/file_io.h"
#include "png/memory_pool.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png::cli {

namespace {

constexpr float kPointsPerInch = 72.0f;
constexpr int kNoAA = FPDF_RENDER_NO_SMOOTHTEXT |
                      FPDF_RENDER_NO_SMOOTHIMAGE |
                      FPDF_RENDER_NO_SMOOTHPATH;

} // anonymous namespace

int RunRaw(const char* pdf_path, float dpi, bool no_aa) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    size_t file_size = 0;
#ifndef _WIN32
    auto* file_data = internal::ReadFileToMemory(pdf_path, file_size);
#else
    auto* file_data = internal::ReadFileToMemoryWin(pdf_path, file_size);
#endif
    if (!file_data) {
        std::fprintf(stderr, "Failed to read: %s\n", pdf_path);
        return 1;
    }

    auto* doc = FPDF_LoadMemDocument64(file_data, file_size, nullptr);
    if (!doc) {
        std::fprintf(stderr, "Failed to open: %s\n", pdf_path);
        std::free(file_data);
        return 1;
    }

    const auto pages = FPDF_GetPageCount(doc);
    for (int i = 0; i < pages; ++i) {
        auto* page = FPDF_LoadPage(doc, i);
        if (!page) continue;

        const auto scale = dpi / kPointsPerInch;
        const auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
        const auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
        if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

        const auto stride = (w * 4 + 63) & ~63;
        const auto buf_size = static_cast<size_t>(stride) * h;

        auto& pool = fast_png::GetProcessLocalPool();
        auto* buffer = pool.Acquire(buf_size);
        if (!buffer) { FPDF_ClosePage(page); continue; }

        auto* bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buffer, stride);
        if (!bitmap) { FPDF_ClosePage(page); continue; }

        int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
        if (no_aa) flags |= kNoAA;
        FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);

        PixelHeader hdr{w, h, stride, 4};
        std::fwrite(&hdr, sizeof(hdr), 1, stdout);
        std::fwrite(buffer, 1, buf_size, stdout);
    }
    std::fflush(stdout);

    FPDF_CloseDocument(doc);
    std::free(file_data);
    return 0;
}

} // namespace fpdf2png::cli
