// fastpdf2png — Single-file render command
// SPDX-License-Identifier: MIT

#include "cli/render_cmd.h"
#include "internal/pdfium_render.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"

namespace fpdf2png::cli {

// ---------------------------------------------------------------------------
// Shared state — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

#ifdef _WIN32
struct SharedState {
    volatile LONG next_page;
    char pad1[60];
    volatile LONG completed_pages;
    char pad2[60];
    int total_pages;
};

inline int ClaimNextPage(SharedState* s)  { return InterlockedExchangeAdd(&s->next_page, 1); }
inline void MarkCompleted(SharedState* s) { InterlockedExchangeAdd(&s->completed_pages, 1); }
inline int GetCompleted(SharedState* s)   { return InterlockedCompareExchange(&s->completed_pages, 0, 0); }
#else
struct alignas(64) SharedState {
    std::atomic<int> next_page{0};
    char pad1[60];
    std::atomic<int> completed_pages{0};
    char pad2[60];
    int total_pages = 0;
};

inline int ClaimNextPage(SharedState* s)  { return s->next_page.fetch_add(1, std::memory_order_relaxed); }
inline void MarkCompleted(SharedState* s) { s->completed_pages.fetch_add(1, std::memory_order_relaxed); }
inline int GetCompleted(SharedState* s)   { return s->completed_pages.load(std::memory_order_relaxed); }
#endif

// ---------------------------------------------------------------------------
// PDFium init helper (local to CLI — same as original)
// ---------------------------------------------------------------------------

namespace {

void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Single-process rendering
// ---------------------------------------------------------------------------

namespace {

int RenderSingle(const char* pdf_path, float dpi, const char* pattern,
                 int pages, int compression, bool no_aa) {
    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) {
        std::fprintf(stderr, "Failed to open: %s\n", pdf_path);
        return 1;
    }

    int rendered = 0;
    for (int i = 0; i < pages; ++i) {
        if (internal::RenderPageToFile(doc, i, dpi, pattern, compression, no_aa))
            ++rendered;
    }

    FPDF_CloseDocument(doc);
    return (rendered == pages) ? 0 : 1;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Multi-process rendering (Unix)
// ---------------------------------------------------------------------------

#ifndef _WIN32

namespace {

[[noreturn]] void WorkerLoop(const char* pdf_path, float dpi, const char* pattern,
                             int compression, SharedState* shared, bool no_aa) {
    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) std::exit(1);

    while (true) {
        const auto page = ClaimNextPage(shared);
        if (page >= shared->total_pages) break;
        if (internal::RenderPageToFile(doc, page, dpi, pattern, compression, no_aa))
            MarkCompleted(shared);
    }

    FPDF_CloseDocument(doc);
    std::exit(0);
}

int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression, bool no_aa) {
    auto* shared = static_cast<SharedState*>(
        mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared == MAP_FAILED) { perror("mmap"); return 1; }

    shared->next_page.store(0);
    shared->completed_pages.store(0);
    shared->total_pages = pages;

    std::vector<pid_t> children;
    children.reserve(workers);

    for (int i = 0; i < workers; ++i) {
        if (auto pid = fork(); pid == 0)
            WorkerLoop(pdf_path, dpi, pattern, compression, shared, no_aa);
        else if (pid > 0)
            children.push_back(pid);
    }

    for (auto pid : children)
        waitpid(pid, nullptr, 0);

    const auto completed = GetCompleted(shared);
    munmap(shared, sizeof(SharedState));
    return (completed == pages) ? 0 : 1;
}

} // anonymous namespace

#endif // !_WIN32

// ---------------------------------------------------------------------------
// Multi-process rendering (Windows) — declaration in render_cmd.h,
// implementation in platform/worker_win.cpp
// ---------------------------------------------------------------------------

#ifdef _WIN32

namespace {

int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression, bool no_aa) {
    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "fastpdf2png_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()));

    auto hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(SharedState), shm_name);
    if (!hMap) {
        std::fprintf(stderr, "CreateFileMapping failed (%lu)\n", GetLastError());
        return 1;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(hMap); return 1; }

    shared->next_page = 0;
    shared->completed_pages = 0;
    shared->total_pages = pages;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    const auto dpi_x10 = static_cast<int>(dpi * 10 + 0.5f);

    std::vector<HANDLE> children;
    children.reserve(workers);

    for (int i = 0; i < workers; ++i) {
        char cmdline[8192];
        std::snprintf(cmdline, sizeof(cmdline),
                      "\"%s\" --worker \"%s\" \"%s\" %d %d \"%s\" %d",
                      exe_path, pdf_path, pattern, dpi_x10, compression, shm_name,
                      no_aa ? 1 : 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            children.push_back(pi.hProcess);
        } else {
            std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n", i, GetLastError());
        }
    }

    if (!children.empty()) {
        auto wait = WaitForMultipleObjects(
            static_cast<DWORD>(children.size()), children.data(), TRUE, INFINITE);
        if (wait == WAIT_FAILED)
            std::fprintf(stderr, "WaitForMultipleObjects failed (%lu)\n", GetLastError());
    }
    for (auto h : children) CloseHandle(h);

    const auto completed = GetCompleted(shared);
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    return (completed == pages) ? 0 : 1;
}

} // anonymous namespace

#endif // _WIN32

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

int RunRender(const ParsedArgs& args) {
    InitPdfium();

    auto* doc = FPDF_LoadDocument(args.pdf_path, nullptr);
    if (!doc) {
        std::fprintf(stderr, "Failed to open PDF: %s (error %lu)\n",
                     args.pdf_path, FPDF_GetLastError());
        FPDF_DestroyLibrary();
        return 1;
    }

    const auto pages = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);

    if (pages <= 0) {
        std::fprintf(stderr, "PDF has no pages\n");
        FPDF_DestroyLibrary();
        return 1;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    const auto result = (args.workers > 1)
        ? RenderMulti(args.pdf_path, args.dpi, args.pattern, pages,
                      args.workers, args.compression, args.no_aa)
        : RenderSingle(args.pdf_path, args.dpi, args.pattern, pages,
                       args.compression, args.no_aa);

    FPDF_DestroyLibrary();

    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (result == 0)
        std::printf("Rendered %d pages in %.3f seconds (%.1f pages/sec)\n",
                    pages, elapsed, pages / elapsed);

    return result;
}

} // namespace fpdf2png::cli
