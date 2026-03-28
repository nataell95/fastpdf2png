// libfastpdf2png — Implementation
// SPDX-License-Identifier: MIT

#define FASTPDF2PNG_BUILD_DLL
#include "lib.h"
#include "png_writer.h"
#include "memory_pool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png {

namespace {

constexpr float kPointsPerInch = 72.0f;
constexpr int kNoAA = FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE |
                      FPDF_RENDER_NO_SMOOTHPATH;

// Read file into memory. Caller must free().
std::pair<uint8_t*, size_t> ReadFile(std::string_view path) {
    std::string p(path);
    auto* f = std::fopen(p.c_str(), "rb");
    if (!f) return {nullptr, 0};
    std::fseek(f, 0, SEEK_END);
    auto size = static_cast<size_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    auto* buf = static_cast<uint8_t*>(std::malloc(size));
    if (!buf) { std::fclose(f); return {nullptr, 0}; }
    size_t read = std::fread(buf, 1, size, f);
    std::fclose(f);
    if (read != size) { std::free(buf); return {nullptr, 0}; }
    return {buf, size};
}

// Render pages from an already-opened doc
std::vector<Page> RenderDoc(FPDF_DOCUMENT doc, int start, int end,
                            float dpi, bool no_aa) {
    std::vector<Page> pages;
    pages.reserve(end - start);

    for (int i = start; i < end; ++i) {
        auto* page = FPDF_LoadPage(doc, i);
        if (!page) continue;

        const auto scale = dpi / kPointsPerInch;
        const auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
        const auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
        if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

        const auto stride = (w * 4 + 63) & ~63;
        const auto buf_size = static_cast<size_t>(stride) * h;
        auto* buffer = static_cast<uint8_t*>(std::malloc(buf_size));
        if (!buffer) { FPDF_ClosePage(page); continue; }

        auto* bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buffer, stride);
        if (!bitmap) { std::free(buffer); FPDF_ClosePage(page); continue; }

        int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
        if (no_aa) flags |= kNoAA;
        FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);

        pages.emplace_back(buffer, w, h, stride);
    }
    return pages;
}

#ifndef _WIN32
// Shared state for multi-process rendering to memory
struct alignas(64) SharedRenderState {
    std::atomic<int> next_page{0};
    int total_pages = 0;
};

// Worker renders pages into shared memory segments
struct SharedPageResult {
    int32_t width, height, stride;
    size_t data_offset;  // offset into shared data buffer
    int32_t valid;       // 1 if rendered successfully
};

// Helper: mmap shared anonymous memory. Returns nullptr on failure.
void* MmapShared(size_t size) {
    auto* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}
#endif

} // namespace

// --- Engine implementation ---

struct Engine::Impl {
    bool initialized = false;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    impl_->initialized = true;
}

Engine::~Engine() {
    if (impl_ && impl_->initialized)
        FPDF_DestroyLibrary();
}

int Engine::page_count(std::string_view pdf_path) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return -1;
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return -1; }
    int n = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    std::free(data);
    return n;
}

int Engine::page_count(std::span<const uint8_t> pdf_data) const {
    auto* doc = FPDF_LoadMemDocument64(pdf_data.data(), pdf_data.size(), nullptr);
    if (!doc) return -1;
    int n = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    return n;
}

RenderResult Engine::render(std::string_view pdf_path, Options opts) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }

    int total = FPDF_GetPageCount(doc);

#ifndef _WIN32
    int workers = opts.workers;

    if (workers > 1 && total > 1) {
        FPDF_CloseDocument(doc);

        auto* shared = static_cast<SharedRenderState*>(
            MmapShared(sizeof(SharedRenderState)));
        if (!shared) { std::free(data); return std::unexpected(Error::AllocFailed); }
        new (&shared->next_page) std::atomic<int>(0);
        shared->total_pages = total;

        auto meta_size = sizeof(SharedPageResult) * total;
        auto* meta = static_cast<SharedPageResult*>(MmapShared(meta_size));
        if (!meta) {
            munmap(shared, sizeof(SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }
        std::memset(meta, 0, meta_size);

        // Compute max page buffer size with overflow protection
        const double pw = static_cast<double>(opts.dpi) / 72.0 * 900.0;
        const double ph = static_cast<double>(opts.dpi) / 72.0 * 1200.0;
        const double max_pg_d = pw * ph * 4.0 + 65536.0;
        if (max_pg_d > 1e12 || max_pg_d * total > 1e12) {
            munmap(meta, meta_size);
            munmap(shared, sizeof(SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }
        const size_t max_page_size = static_cast<size_t>(max_pg_d);
        const size_t data_buf_size = max_page_size * total;

        auto* data_buf = static_cast<uint8_t*>(MmapShared(data_buf_size));
        if (!data_buf) {
            munmap(meta, meta_size);
            munmap(shared, sizeof(SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }

        std::vector<pid_t> children;
        workers = std::min(workers, total);
        for (int i = 0; i < workers; ++i) {
            auto pid = fork();
            if (pid == 0) {
                auto* child_doc = FPDF_LoadMemDocument64(data, size, nullptr);
                if (!child_doc) _exit(1);

                while (true) {
                    int pg = shared->next_page.fetch_add(1, std::memory_order_relaxed);
                    if (pg >= total) break;

                    auto* page = FPDF_LoadPage(child_doc, pg);
                    if (!page) continue;

                    const auto scale = opts.dpi / kPointsPerInch;
                    const auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
                    const auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
                    if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

                    const auto stride = (w * 4 + 63) & ~63;
                    const auto buf_size_pg = static_cast<size_t>(stride) * h;
                    const auto offset = static_cast<size_t>(pg) * max_page_size;

                    if (offset + buf_size_pg > data_buf_size) {
                        FPDF_ClosePage(page);
                        continue;
                    }

                    auto* buffer = data_buf + offset;
                    auto* bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buffer, stride);
                    if (!bitmap) { FPDF_ClosePage(page); continue; }

                    int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
                    if (opts.no_aa) flags |= kNoAA;
                    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
                    FPDFBitmap_Destroy(bitmap);
                    FPDF_ClosePage(page);

                    meta[pg] = {w, h, stride, offset, 1};
                }

                FPDF_CloseDocument(child_doc);
                _exit(0);
            } else if (pid > 0) {
                children.push_back(pid);
            }
        }

        for (auto pid : children)
            waitpid(pid, nullptr, 0);

        // Collect results in page order
        std::vector<Page> pages;
        pages.reserve(total);
        for (int i = 0; i < total; ++i) {
            if (meta[i].valid) {
                auto buf_size_pg = static_cast<size_t>(meta[i].stride) * meta[i].height;
                auto* copy = static_cast<uint8_t*>(std::malloc(buf_size_pg));
                if (!copy) continue;  // skip page on OOM
                std::memcpy(copy, data_buf + meta[i].data_offset, buf_size_pg);
                pages.emplace_back(copy, meta[i].width, meta[i].height, meta[i].stride);
            }
        }

        munmap(data_buf, data_buf_size);
        munmap(meta, meta_size);
        munmap(shared, sizeof(SharedRenderState));
        std::free(data);
        return pages;
    }
#endif

    // Single-process fallback
    auto pages = RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

RenderResult Engine::render(std::span<const uint8_t> pdf_data, Options opts) const {
    auto* doc = FPDF_LoadMemDocument64(pdf_data.data(), pdf_data.size(), nullptr);
    if (!doc) return std::unexpected(Error::InvalidPdf);
    int total = FPDF_GetPageCount(doc);
    auto pages = RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    return pages;
}

RenderResult Engine::render_pages(std::string_view pdf_path,
                                   int start, int end, Options opts) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }
    auto pages = RenderDoc(doc, start, end, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

// --- process_many: fork workers, each grabs PDFs and calls user callback ---

#ifndef _WIN32
int Engine::process_many(const std::vector<std::string>& pdf_paths,
                          Options opts, int num_workers,
                          PageCallback callback) const {
    const int total = static_cast<int>(pdf_paths.size());
    if (total == 0) return 0;
    num_workers = std::clamp(num_workers, 1, total);

    if (num_workers <= 1) {
        int done = 0;
        for (auto& path : pdf_paths) {
            auto [data, size] = ReadFile(path);
            if (!data) continue;
            auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
            if (!doc) { std::free(data); continue; }
            auto pages = RenderDoc(doc, 0, FPDF_GetPageCount(doc), opts.dpi, opts.no_aa);
            FPDF_CloseDocument(doc);
            std::free(data);
            callback(path, pages);
            ++done;
        }
        return done;
    }

    // All counters in mmap shared memory — visible across fork
    struct alignas(64) SharedState {
        std::atomic<int> next{0};
        std::atomic<int> done_pdfs{0};
        std::atomic<int> done_pages{0};
    };

    auto* shared = static_cast<SharedState*>(MmapShared(sizeof(SharedState)));
    if (!shared) return 0;
    new (&shared->next) std::atomic<int>(0);
    new (&shared->done_pdfs) std::atomic<int>(0);
    new (&shared->done_pages) std::atomic<int>(0);

    std::vector<pid_t> children;
    children.reserve(num_workers);

    for (int i = 0; i < num_workers; ++i) {
        auto pid = fork();
        if (pid == 0) {
            while (true) {
                int idx = shared->next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= total) break;

                auto [fdata, fsize] = ReadFile(pdf_paths[idx]);
                if (!fdata) continue;
                auto* doc = FPDF_LoadMemDocument64(fdata, fsize, nullptr);
                if (!doc) { std::free(fdata); continue; }

                auto pages = RenderDoc(doc, 0, FPDF_GetPageCount(doc),
                                       opts.dpi, opts.no_aa);
                FPDF_CloseDocument(doc);
                std::free(fdata);

                callback(pdf_paths[idx], pages);

                shared->done_pdfs.fetch_add(1, std::memory_order_relaxed);
                shared->done_pages.fetch_add(
                    static_cast<int>(pages.size()), std::memory_order_relaxed);
            }
            _exit(0);
        } else if (pid > 0) {
            children.push_back(pid);
        }
    }

    for (auto pid : children)
        waitpid(pid, nullptr, 0);

    int result = shared->done_pdfs.load(std::memory_order_acquire);
    munmap(shared, sizeof(SharedState));
    return result;
}
#else
int Engine::process_many(const std::vector<std::string>& pdf_paths,
                          Options opts, int num_workers,
                          PageCallback callback) const {
    int done = 0;
    for (auto& path : pdf_paths) {
        auto result = render(path, opts);
        if (result) {
            callback(path, *result);
            ++done;
        }
    }
    return done;
}
#endif

// --- Pool: persistent forked workers with pipe IPC ---

#ifndef _WIN32

namespace {

struct PoolJob {
    char pdf_path[512];
    float dpi;
    bool no_aa;
    bool shutdown;  // true = worker should exit
};

struct PoolResult {
    char pdf_path[512];
    int pages_rendered;
};

[[noreturn]] void PoolWorkerLoop(int cmd_fd, int result_fd,
                                  Pool::Callback default_cb) {
    // Worker has its own PDFium via COW from parent fork
    PoolJob job{};
    while (true) {
        // Block on pipe — zero CPU when idle
        auto* p = reinterpret_cast<uint8_t*>(&job);
        size_t remaining = sizeof(job);
        while (remaining > 0) {
            auto n = read(cmd_fd, p, remaining);
            if (n <= 0) goto done;
            p += n;
            remaining -= n;
        }

        if (job.shutdown) break;

        auto [fdata, fsize] = ReadFile(job.pdf_path);
        FPDF_DOCUMENT doc = fdata ? FPDF_LoadMemDocument64(fdata, fsize, nullptr) : nullptr;

        PoolResult result{};
        std::strncpy(result.pdf_path, job.pdf_path, sizeof(result.pdf_path) - 1);

        if (doc) {
            auto pages = RenderDoc(doc, 0, FPDF_GetPageCount(doc), job.dpi, job.no_aa);
            FPDF_CloseDocument(doc);
            result.pages_rendered = static_cast<int>(pages.size());
            if (default_cb) default_cb(job.pdf_path, pages);
        }
        std::free(fdata);

        // Send result back (non-blocking for parent to collect)
        auto* rp = reinterpret_cast<const uint8_t*>(&result);
        size_t rrem = sizeof(result);
        while (rrem > 0) {
            auto n = write(result_fd, rp, rrem);
            if (n <= 0) goto done;
            rp += n;
            rrem -= n;
        }
    }

done:
    close(cmd_fd);
    close(result_fd);
    _exit(0);
}

} // namespace

struct Pool::Impl {
    struct Worker {
        pid_t pid;
        int cmd_fd;
        int result_fd;
        int in_flight;
    };

    std::vector<Worker> workers;
    Options opts;
    Callback default_callback;
    int submitted = 0;
    int completed_count = 0;

    // Pick worker with fewest in-flight jobs
    int pick_worker() {
        int best = 0;
        for (int i = 1; i < static_cast<int>(workers.size()); ++i)
            if (workers[i].in_flight < workers[best].in_flight) best = i;
        return best;
    }

    // Drain ready results (non-blocking)
    void drain() {
        std::vector<struct pollfd> pfds(workers.size());
        for (size_t i = 0; i < workers.size(); ++i)
            pfds[i] = {workers[i].result_fd, POLLIN, 0};

        while (true) {
            int ready = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 0);
            if (ready <= 0) break;
            for (size_t i = 0; i < workers.size(); ++i) {
                if (pfds[i].revents & POLLIN) {
                    PoolResult result{};
                    auto* p = reinterpret_cast<uint8_t*>(&result);
                    size_t rem = sizeof(result);
                    bool ok = true;
                    while (rem > 0) {
                        auto n = read(workers[i].result_fd, p, rem);
                        if (n <= 0) { ok = false; break; }
                        p += n;
                        rem -= n;
                    }
                    if (ok) {
                        workers[i].in_flight--;
                        completed_count++;
                    }
                }
            }
        }
    }
};

Pool::Pool(int num_workers, Options opts, Callback callback)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;
    impl_->default_callback = std::move(callback);

    // Init PDFium in parent so workers inherit it via fork COW
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    num_workers = std::max(1, num_workers);
    impl_->workers.resize(num_workers);

    for (int i = 0; i < num_workers; ++i) {
        int cmd_pipe[2], result_pipe[2];
        if (pipe(cmd_pipe) != 0 || pipe(result_pipe) != 0) continue;

        auto pid = fork();
        if (pid == 0) {
            // Child
            close(cmd_pipe[1]);
            close(result_pipe[0]);
            for (int j = 0; j < i; ++j) {
                close(impl_->workers[j].cmd_fd);
                close(impl_->workers[j].result_fd);
            }
            PoolWorkerLoop(cmd_pipe[0], result_pipe[1], impl_->default_callback);
        }

        close(cmd_pipe[0]);
        close(result_pipe[1]);
        impl_->workers[i] = {pid, cmd_pipe[1], result_pipe[0], 0};
    }
}

Pool::~Pool() {
    // Send shutdown to all workers
    for (auto& w : impl_->workers) {
        PoolJob job{};
        job.shutdown = true;
        write(w.cmd_fd, &job, sizeof(job));
        close(w.cmd_fd);
    }
    for (auto& w : impl_->workers) {
        close(w.result_fd);
        waitpid(w.pid, nullptr, 0);
    }
    FPDF_DestroyLibrary();
}

void Pool::submit(const std::string& pdf_path) {
    // Drain completed results first to keep in_flight counts accurate
    impl_->drain();

    PoolJob job{};
    std::strncpy(job.pdf_path, pdf_path.c_str(), sizeof(job.pdf_path) - 1);
    job.dpi = impl_->opts.dpi;
    job.no_aa = impl_->opts.no_aa;
    job.shutdown = false;

    int w = impl_->pick_worker();
    auto* p = reinterpret_cast<const uint8_t*>(&job);
    size_t rem = sizeof(job);
    while (rem > 0) {
        auto n = write(impl_->workers[w].cmd_fd, p, rem);
        if (n <= 0) break;
        p += n;
        rem -= n;
    }
    impl_->workers[w].in_flight++;
    impl_->submitted++;
}

void Pool::submit(const std::string& pdf_path, Callback) {
    // Per-job callback not supported in fork model — use default callback
    submit(pdf_path);
}

void Pool::wait() {
    // Block-read remaining results using poll
    while (impl_->completed_count < impl_->submitted) {
        std::vector<struct pollfd> pfds(impl_->workers.size());
        for (size_t i = 0; i < impl_->workers.size(); ++i)
            pfds[i] = {impl_->workers[i].result_fd, POLLIN, 0};

        int ready = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 5000);
        if (ready <= 0) continue;

        for (size_t i = 0; i < impl_->workers.size(); ++i) {
            if (pfds[i].revents & POLLIN) {
                PoolResult result{};
                auto* p = reinterpret_cast<uint8_t*>(&result);
                size_t rem = sizeof(result);
                bool ok = true;
                while (rem > 0) {
                    auto n = read(impl_->workers[i].result_fd, p, rem);
                    if (n <= 0) { ok = false; break; }
                    p += n;
                    rem -= n;
                }
                if (ok) {
                    impl_->workers[i].in_flight--;
                    impl_->completed_count++;
                }
            }
        }
    }
}

int Pool::completed() const {
    return impl_->completed_count;
}

#endif

// --- C API ---

static Engine* g_engine = nullptr;

extern "C" {

void fpdf2png_init() {
    if (!g_engine) g_engine = new Engine();
}

void fpdf2png_shutdown() {
    delete g_engine;
    g_engine = nullptr;
}

int fpdf2png_page_count(const char* path) {
    if (!g_engine) fpdf2png_init();
    return g_engine->page_count(path);
}

int fpdf2png_render(const char* path, float dpi, int no_aa,
                     fpdf2png_page_c** out, int* count) {
    if (!g_engine) fpdf2png_init();
    Options opts{dpi, no_aa != 0};
    auto result = g_engine->render(path, opts);
    if (!result) { *out = nullptr; *count = 0; return static_cast<int>(result.error()); }

    auto& pages = *result;
    *count = static_cast<int>(pages.size());
    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * pages.size()));
    if (!*out) { *count = 0; return static_cast<int>(Error::AllocFailed); }
    for (size_t i = 0; i < pages.size(); ++i) {
        (*out)[i] = {pages[i].data.release(), pages[i].width, pages[i].height, pages[i].stride};
    }
    return 0;
}

int fpdf2png_render_mem(const uint8_t* data, size_t size, float dpi, int no_aa,
                         fpdf2png_page_c** out, int* count) {
    if (!g_engine) fpdf2png_init();
    Options opts{dpi, no_aa != 0};
    auto result = g_engine->render({data, size}, opts);
    if (!result) { *out = nullptr; *count = 0; return static_cast<int>(result.error()); }

    auto& pages = *result;
    *count = static_cast<int>(pages.size());
    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * pages.size()));
    if (!*out) { *count = 0; return static_cast<int>(Error::AllocFailed); }
    for (size_t i = 0; i < pages.size(); ++i) {
        (*out)[i] = {pages[i].data.release(), pages[i].width, pages[i].height, pages[i].stride};
    }
    return 0;
}

void fpdf2png_free(fpdf2png_page_c* pages, int count) {
    if (!pages) return;
    for (int i = 0; i < count; ++i)
        std::free(pages[i].data);
    std::free(pages);
}

} // extern "C"

} // namespace fpdf2png
