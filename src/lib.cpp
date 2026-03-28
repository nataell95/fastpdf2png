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

// --- Pool: persistent workers, streaming results via shared memory ---
//
// submit() → job queue (shared mmap + wake pipe) → workers steal jobs →
// render into shared pixel buffer → write result metadata to result pipe →
// next() reads one result at a time, copies pixels out, returns PoolResult.

#ifndef _WIN32

namespace {

constexpr int kMaxPoolJobs = 16384;
// Max pixel bytes per page slot (enough for A3 at 300 DPI)
constexpr size_t kMaxPixelsPerPage = 64ULL * 1024 * 1024;  // 64 MB
constexpr int kMaxPagesPerPdf = 256;

struct alignas(64) PoolShared {
    std::atomic<int> job_tail{0};   // parent writes
    char pad1[60];
    std::atomic<int> job_head{0};   // workers read (steal)
    char pad2[60];
    std::atomic<int> done{0};
    char pad3[60];
    // Per-slot pixel buffer offset allocator (bump allocator)
    std::atomic<size_t> pixel_alloc{0};
};

struct PoolJobSlot {
    char pdf_path[512];
    float dpi;
    bool no_aa;
};

// Written by worker to result pipe — small, fixed size
struct PoolResultMeta {
    char pdf_path[512];
    int num_pages;
    struct PageMeta {
        int32_t width, height, stride;
        size_t pixel_offset;  // into shared pixel buffer
    } pages[kMaxPagesPerPdf];
};

} // namespace

struct Pool::Impl {
    PoolShared* shared = nullptr;
    PoolJobSlot* job_slots = nullptr;
    uint8_t* pixel_buf = nullptr;
    size_t pixel_buf_size = 0;

    int wake_pipe[2] = {-1, -1};

    struct WorkerInfo {
        pid_t pid;
        int result_rd;  // per-worker result pipe (read end)
    };
    std::vector<WorkerInfo> workers;
    Options opts;
    int submit_count = 0;
    int complete_count = 0;
    bool finished = false;
};

Pool::Pool(int num_workers, Options opts)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;

    impl_->shared = static_cast<PoolShared*>(MmapShared(sizeof(PoolShared)));
    if (!impl_->shared) return;
    new (&impl_->shared->job_tail) std::atomic<int>(0);
    new (&impl_->shared->job_head) std::atomic<int>(0);
    new (&impl_->shared->done) std::atomic<int>(0);
    new (&impl_->shared->pixel_alloc) std::atomic<size_t>(0);

    impl_->job_slots = static_cast<PoolJobSlot*>(
        MmapShared(sizeof(PoolJobSlot) * kMaxPoolJobs));
    if (!impl_->job_slots) {
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    // Shared pixel buffer — 2 GB max (handles ~200 pages at 300 DPI)
    impl_->pixel_buf_size = 2ULL * 1024 * 1024 * 1024;
    impl_->pixel_buf = static_cast<uint8_t*>(MmapShared(impl_->pixel_buf_size));
    if (!impl_->pixel_buf) {
        // Try smaller: 512 MB
        impl_->pixel_buf_size = 512ULL * 1024 * 1024;
        impl_->pixel_buf = static_cast<uint8_t*>(MmapShared(impl_->pixel_buf_size));
    }
    if (!impl_->pixel_buf) {
        munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    if (pipe(impl_->wake_pipe) != 0) {
        munmap(impl_->pixel_buf, impl_->pixel_buf_size);
        munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    num_workers = std::max(1, num_workers);
    auto* shared = impl_->shared;
    auto* job_slots = impl_->job_slots;
    auto* pixel_buf = impl_->pixel_buf;
    auto pixel_buf_size = impl_->pixel_buf_size;
    int wake_rd = impl_->wake_pipe[0];

    for (int i = 0; i < num_workers; ++i) {
        int result_pipe[2];
        if (pipe(result_pipe) != 0) continue;

        auto pid = fork();
        if (pid == 0) {
            close(impl_->wake_pipe[1]);
            close(result_pipe[0]);
            // Close other workers' result pipes
            for (auto& w : impl_->workers)
                close(w.result_rd);

            int result_wr = result_pipe[1];

            while (true) {
                char c;
                if (read(wake_rd, &c, 1) <= 0) break;

                int idx = shared->job_head.fetch_add(1, std::memory_order_acquire);
                if (idx >= shared->job_tail.load(std::memory_order_acquire)) continue;

                auto& job = job_slots[idx % kMaxPoolJobs];
                PoolResultMeta meta{};
                std::strncpy(meta.pdf_path, job.pdf_path, sizeof(meta.pdf_path) - 1);

                auto [fdata, fsize] = ReadFile(job.pdf_path);
                if (fdata) {
                    auto* doc = FPDF_LoadMemDocument64(fdata, fsize, nullptr);
                    if (doc) {
                        int npages = FPDF_GetPageCount(doc);
                        int rendered = 0;
                        for (int p = 0; p < npages && rendered < kMaxPagesPerPdf; ++p) {
                            auto* page = FPDF_LoadPage(doc, p);
                            if (!page) continue;

                            auto scale = job.dpi / kPointsPerInch;
                            auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
                            auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
                            if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

                            auto stride = (w * 4 + 63) & ~63;
                            auto pg_size = static_cast<size_t>(stride) * h;

                            auto offset = shared->pixel_alloc.fetch_add(
                                pg_size, std::memory_order_relaxed);
                            if (offset + pg_size > pixel_buf_size) {
                                FPDF_ClosePage(page);
                                continue;
                            }

                            auto* buf = pixel_buf + offset;
                            auto* bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buf, stride);
                            if (!bmp) { FPDF_ClosePage(page); continue; }

                            int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
                            if (job.no_aa) flags |= kNoAA;
                            FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
                            FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, flags);
                            FPDFBitmap_Destroy(bmp);
                            FPDF_ClosePage(page);

                            meta.pages[rendered] = {w, h, stride, offset};
                            rendered++;
                        }
                        meta.num_pages = rendered;
                        FPDF_CloseDocument(doc);
                    }
                    std::free(fdata);
                }

                // Per-worker pipe — no interleaving
                auto* mp = reinterpret_cast<const uint8_t*>(&meta);
                size_t mrem = sizeof(meta);
                while (mrem > 0) {
                    auto n = write(result_wr, mp, mrem);
                    if (n <= 0) break;
                    mp += n;
                    mrem -= n;
                }
                shared->done.fetch_add(1, std::memory_order_release);
            }

            close(wake_rd);
            close(result_wr);
            _exit(0);
        }

        close(result_pipe[1]);
        impl_->workers.push_back({pid, result_pipe[0]});
    }
}

Pool::~Pool() {
    if (!impl_->shared) return;

    // Close wake pipe — workers exit on EOF
    close(impl_->wake_pipe[1]);
    close(impl_->wake_pipe[0]);

    for (auto& w : impl_->workers) {
        close(w.result_rd);
        waitpid(w.pid, nullptr, 0);
    }

    munmap(impl_->pixel_buf, impl_->pixel_buf_size);
    munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
    munmap(impl_->shared, sizeof(PoolShared));
    FPDF_DestroyLibrary();
}

void Pool::submit(const std::string& pdf_path) {
    if (!impl_->shared || impl_->finished) return;

    int idx = impl_->submit_count;
    auto& slot = impl_->job_slots[idx % kMaxPoolJobs];
    std::strncpy(slot.pdf_path, pdf_path.c_str(), sizeof(slot.pdf_path) - 1);
    slot.pdf_path[sizeof(slot.pdf_path) - 1] = '\0';
    slot.dpi = impl_->opts.dpi;
    slot.no_aa = impl_->opts.no_aa;

    impl_->shared->job_tail.store(idx + 1, std::memory_order_release);
    impl_->submit_count++;

    char c = 1;
    write(impl_->wake_pipe[1], &c, 1);
}

std::optional<PoolResult> Pool::next() {
    if (!impl_->shared) return std::nullopt;
    if (impl_->complete_count >= impl_->submit_count) return std::nullopt;

    // Reset pixel buffer when we've caught up with all completed work.
    // At this point all pixels from prior results have been copied out by next(),
    // and no worker is writing (done == complete_count means all finished results
    // have been read). Workers currently rendering will allocate from offset 0 again.
    int done_now = impl_->shared->done.load(std::memory_order_acquire);
    if (done_now > 0 && done_now == impl_->complete_count) {
        impl_->shared->pixel_alloc.store(0, std::memory_order_release);
    }

    // Poll all worker result pipes — block until one has data
    auto nw = static_cast<int>(impl_->workers.size());
    std::vector<struct pollfd> pfds(nw);
    for (int i = 0; i < nw; ++i)
        pfds[i] = {impl_->workers[i].result_rd, POLLIN, 0};

    while (true) {
        int ready = poll(pfds.data(), static_cast<nfds_t>(nw), 5000);
        if (ready <= 0) continue;  // timeout, retry

        for (int i = 0; i < nw; ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            // Read full result metadata from this worker
            PoolResultMeta meta{};
            auto* p = reinterpret_cast<uint8_t*>(&meta);
            size_t rem = sizeof(meta);
            while (rem > 0) {
                auto n = read(impl_->workers[i].result_rd, p, rem);
                if (n <= 0) return std::nullopt;
                p += n;
                rem -= n;
            }

            impl_->complete_count++;

            // Copy pixels out of shared memory
            PoolResult result;
            result.pdf_path = meta.pdf_path;
            result.pages.reserve(meta.num_pages);

            for (int j = 0; j < meta.num_pages; ++j) {
                auto& pm = meta.pages[j];
                auto pg_size = static_cast<size_t>(pm.stride) * pm.height;
                auto* copy = static_cast<uint8_t*>(std::malloc(pg_size));
                if (!copy) continue;
                std::memcpy(copy, impl_->pixel_buf + pm.pixel_offset, pg_size);
                result.pages.emplace_back(copy, pm.width, pm.height, pm.stride);
            }

            return result;
        }
    }
}

void Pool::finish() {
    impl_->finished = true;
}

int Pool::submitted() const { return impl_->submit_count; }
int Pool::completed() const { return impl_->complete_count; }

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
