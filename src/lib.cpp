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
#include <mutex>
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

// --- Pool: persistent workers, hybrid shared-memory + pipe results ---
//
// Workers render into shared memory (per-page slots from atomic allocator),
// send lightweight metadata through per-worker pipes.
// Parent reads metadata, copies pixels from shared memory, frees slots.
// Atomic slot free-list ensures no race conditions.

#ifndef _WIN32

namespace {

constexpr int kMaxPoolJobs = 16384;
constexpr int kMaxPagesPerResult = 512;

// Shared page slot pool — workers allocate, parent frees after copying
constexpr size_t kPageSlotSize = 48ULL * 1024 * 1024;  // 48MB max per page
constexpr int kNumPageSlots = 64;

struct alignas(64) PoolShared {
    std::atomic<int> job_tail{0};
    char pad1[60];
    std::atomic<int> job_head{0};
    char pad2[60];
    // Page slot free stack (lock-free)
    std::atomic<int> slot_stack[kNumPageSlots];
    std::atomic<int> slot_top{0};  // index into slot_stack
};

struct PoolJobSlot {
    char pdf_path[512];
    float dpi;
    bool no_aa;
};

// Lightweight result metadata sent through pipe (no pixel data)
struct PoolResultHdr {
    char pdf_path[512];
    int32_t num_pages;
    struct PageInfo {
        int32_t width, height, stride;
        int32_t slot_idx;  // index into shared page slot pool (-1 = skipped)
    } pages[kMaxPagesPerResult];
};

} // namespace

struct Pool::Impl {
    PoolShared* shared = nullptr;
    PoolJobSlot* job_slots = nullptr;
    uint8_t* page_slots = nullptr;  // kNumPageSlots × kPageSlotSize
    int wake_pipe[2] = {-1, -1};

    struct WorkerInfo { pid_t pid; int result_rd; };
    std::vector<WorkerInfo> workers;
    Options opts;
    std::atomic<int> submit_count{0};
    std::atomic<int> complete_count{0};
    std::atomic<bool> finished{false};
    std::mutex submit_mtx;  // protects job_slots write + submit_count increment
    std::mutex next_mtx;    // protects pipe reads + complete_count increment
};

Pool::Pool(int num_workers, Options opts)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;

    impl_->shared = static_cast<PoolShared*>(MmapShared(sizeof(PoolShared)));
    if (!impl_->shared) return;
    new (&impl_->shared->job_tail) std::atomic<int>(0);
    new (&impl_->shared->job_head) std::atomic<int>(0);
    // Initialize free slot stack: all slots available
    new (&impl_->shared->slot_top) std::atomic<int>(kNumPageSlots);
    for (int i = 0; i < kNumPageSlots; ++i) {
        new (&impl_->shared->slot_stack[i]) std::atomic<int>(i);
    }

    impl_->job_slots = static_cast<PoolJobSlot*>(
        MmapShared(sizeof(PoolJobSlot) * kMaxPoolJobs));
    if (!impl_->job_slots) {
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    // Shared page pixel buffer
    auto page_buf_size = static_cast<size_t>(kNumPageSlots) * kPageSlotSize;
    impl_->page_slots = static_cast<uint8_t*>(MmapShared(page_buf_size));
    if (!impl_->page_slots) {
        munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    if (pipe(impl_->wake_pipe) != 0) {
        munmap(impl_->page_slots, page_buf_size);
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
    auto* page_slots = impl_->page_slots;
    int wake_rd = impl_->wake_pipe[0];

    for (int i = 0; i < num_workers; ++i) {
        int result_pipe[2];
        if (pipe(result_pipe) != 0) continue;

        auto pid = fork();
        if (pid == 0) {
            close(impl_->wake_pipe[1]);
            close(result_pipe[0]);
            for (auto& w : impl_->workers)
                close(w.result_rd);

            int rfd = result_pipe[1];

            // Try to alloc a page slot. Returns -1 if none available.
            auto try_alloc_slot = [&]() -> int {
                int top = shared->slot_top.load(std::memory_order_acquire);
                while (top > 0) {
                    if (shared->slot_top.compare_exchange_weak(
                            top, top - 1, std::memory_order_acq_rel))
                        return shared->slot_stack[top - 1].load(std::memory_order_relaxed);
                }
                return -1;  // no slots available
            };

            auto pipe_write = [](int fd, const void* buf, size_t count) -> bool {
                auto* p = static_cast<const uint8_t*>(buf);
                while (count > 0) {
                    auto n = write(fd, p, count);
                    if (n <= 0) return false;
                    p += n; count -= n;
                }
                return true;
            };

            while (true) {
                char c;
                if (read(wake_rd, &c, 1) <= 0) break;

                int idx = shared->job_head.fetch_add(1, std::memory_order_acquire);
                if (idx >= shared->job_tail.load(std::memory_order_acquire)) continue;

                auto& job = job_slots[idx % kMaxPoolJobs];

                // Render all pages into local memory (RenderDoc mallocs each page)
                auto [fdata, fsize] = ReadFile(job.pdf_path);
                std::vector<Page> pages;
                if (fdata) {
                    auto* doc = FPDF_LoadMemDocument64(fdata, fsize, nullptr);
                    if (doc) {
                        pages = RenderDoc(doc, 0, FPDF_GetPageCount(doc),
                                          job.dpi, job.no_aa);
                        FPDF_CloseDocument(doc);
                    }
                    std::free(fdata);
                }

                // Build header, copy pages to shared slots where possible
                PoolResultHdr hdr{};
                std::strncpy(hdr.pdf_path, job.pdf_path, sizeof(hdr.pdf_path) - 1);
                hdr.num_pages = static_cast<int32_t>(
                    std::min(pages.size(), static_cast<size_t>(kMaxPagesPerResult)));

                for (int p = 0; p < hdr.num_pages; ++p) {
                    auto& pg = pages[p];
                    auto pg_size = static_cast<size_t>(pg.stride) * pg.height;
                    int slot = (pg_size <= kPageSlotSize) ? try_alloc_slot() : -1;

                    if (slot >= 0) {
                        // Copy into shared memory slot
                        std::memcpy(
                            page_slots + static_cast<size_t>(slot) * kPageSlotSize,
                            pg.data.get(), pg_size);
                    }
                    hdr.pages[p] = {pg.width, pg.height, pg.stride, slot};
                }

                // Send header
                pipe_write(rfd, &hdr, sizeof(hdr));

                // For pages without shared slots, send pixels through pipe
                for (int p = 0; p < hdr.num_pages; ++p) {
                    if (hdr.pages[p].slot_idx < 0) {
                        auto& pg = pages[p];
                        auto pg_size = static_cast<size_t>(pg.stride) * pg.height;
                        pipe_write(rfd, pg.data.get(), pg_size);
                    }
                }
            }

            close(wake_rd);
            close(rfd);
            _exit(0);
        }

        close(result_pipe[1]);
        impl_->workers.push_back({pid, result_pipe[0]});
    }
}

Pool::~Pool() {
    if (!impl_->shared) return;
    close(impl_->wake_pipe[1]);
    close(impl_->wake_pipe[0]);
    for (auto& w : impl_->workers) {
        close(w.result_rd);
        waitpid(w.pid, nullptr, 0);
    }
    auto page_buf_size = static_cast<size_t>(kNumPageSlots) * kPageSlotSize;
    munmap(impl_->page_slots, page_buf_size);
    munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
    munmap(impl_->shared, sizeof(PoolShared));
    FPDF_DestroyLibrary();
}

void Pool::submit(const std::string& pdf_path) {
    std::lock_guard<std::mutex> lock(impl_->submit_mtx);
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
    std::lock_guard<std::mutex> lock(impl_->next_mtx);
    if (!impl_->shared) return std::nullopt;
    if (impl_->complete_count >= impl_->submit_count) return std::nullopt;

    auto nw = static_cast<int>(impl_->workers.size());
    std::vector<struct pollfd> pfds(nw);
    for (int i = 0; i < nw; ++i)
        pfds[i] = {impl_->workers[i].result_rd, POLLIN, 0};

    while (true) {
        int ready = poll(pfds.data(), static_cast<nfds_t>(nw), 5000);
        if (ready <= 0) continue;

        for (int i = 0; i < nw; ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            int fd = impl_->workers[i].result_rd;

            PoolResultHdr hdr{};
            auto* p = reinterpret_cast<uint8_t*>(&hdr);
            size_t rem = sizeof(hdr);
            while (rem > 0) {
                auto n = read(fd, p, rem);
                if (n <= 0) return std::nullopt;
                p += n;
                rem -= n;
            }

            auto pipe_read = [](int f, void* buf, size_t count) -> bool {
                auto* p = static_cast<uint8_t*>(buf);
                while (count > 0) {
                    auto n = read(f, p, count);
                    if (n <= 0) return false;
                    p += n; count -= n;
                }
                return true;
            };

            PoolResult result;
            result.pdf_path = hdr.pdf_path;
            result.pages.reserve(hdr.num_pages);

            for (int j = 0; j < hdr.num_pages; ++j) {
                auto& pi = hdr.pages[j];
                auto pg_size = static_cast<size_t>(pi.stride) * pi.height;
                auto* copy = static_cast<uint8_t*>(std::malloc(pg_size));
                if (!copy) continue;

                if (pi.slot_idx >= 0) {
                    // Copy from shared memory, free slot
                    std::memcpy(copy,
                                impl_->page_slots + static_cast<size_t>(pi.slot_idx) * kPageSlotSize,
                                pg_size);
                    int top = impl_->shared->slot_top.fetch_add(1, std::memory_order_acq_rel);
                    impl_->shared->slot_stack[top].store(pi.slot_idx, std::memory_order_release);
                } else {
                    // Read pixels from pipe
                    if (!pipe_read(fd, copy, pg_size)) { std::free(copy); break; }
                }
                result.pages.emplace_back(copy, pi.width, pi.height, pi.stride);
            }

            impl_->complete_count++;
            return result;
        }
    }
}

void Pool::finish() {
    std::lock_guard<std::mutex> lock(impl_->submit_mtx);
    impl_->finished = true;
}

int Pool::submitted() const {
    return impl_->submit_count.load(std::memory_order_acquire);
}

int Pool::completed() const {
    return impl_->complete_count.load(std::memory_order_acquire);
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
