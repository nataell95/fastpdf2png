// fastpdf2png — Unix multi-process rendering (fork-based)
// SPDX-License-Identifier: MIT

#ifndef _WIN32

#include "cli/platform/render_platform.h"
#include "cli/shared_cmd.h"
#include "internal/pdfium_render.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fpdfview.h"

namespace fpdf2png::cli {

// ---------------------------------------------------------------------------
// Shared state — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

struct alignas(64) SharedState {
    std::atomic<int> next_page{0};
    char pad1[60];
    std::atomic<int> completed_pages{0};
    char pad2[60];
    int total_pages = 0;
};

static int ClaimNextPage(SharedState* s)  { return s->next_page.fetch_add(1, std::memory_order_relaxed); }
static void MarkCompleted(SharedState* s) { s->completed_pages.fetch_add(1, std::memory_order_relaxed); }
static int GetCompleted(SharedState* s)   { return s->completed_pages.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Single-process rendering
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Multi-process rendering (Unix)
// ---------------------------------------------------------------------------

namespace {

// A forked worker leaves with _exit, never exit(): exit() would run the
// parent's atexit handlers and flush the parent's stdio buffers a second time,
// and in daemon mode stdout is the command pipe back to the caller.
[[noreturn]] void WorkerLoop(const char* pdf_path, float dpi, const char* pattern,
                             int compression, SharedState* shared, bool no_aa) {
    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) _exit(1);

    while (true) {
        const auto page = ClaimNextPage(shared);
        if (page >= shared->total_pages) break;
        if (internal::RenderPageToFile(doc, page, dpi, pattern, compression, no_aa))
            MarkCompleted(shared);
    }

    FPDF_CloseDocument(doc);
    _exit(0);
}

} // anonymous namespace

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
        else
            perror("fork");
    }

    for (auto pid : children)
        waitpid(pid, nullptr, 0);

    // Pages nobody produced (every fork failed, or a worker died mid-way) are
    // rendered here in-process instead of failing the whole document.
    if (GetCompleted(shared) < pages) {
        auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
        if (doc) {
            char probe[4096];
            for (int page = 0; page < pages; ++page) {
                const auto n = std::snprintf(probe, sizeof(probe), pattern, page + 1);
                if (n < 0 || static_cast<size_t>(n) >= sizeof(probe)) continue;
                if (access(probe, F_OK) == 0) continue;
                if (internal::RenderPageToFile(doc, page, dpi, pattern, compression, no_aa))
                    MarkCompleted(shared);
            }
            FPDF_CloseDocument(doc);
        }
    }

    const auto completed = GetCompleted(shared);
    munmap(shared, sizeof(SharedState));
    return (completed == pages) ? 0 : 1;
}

} // namespace fpdf2png::cli

#endif // !_WIN32
