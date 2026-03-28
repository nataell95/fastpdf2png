// libfastpdf2png — Modern C++20 API for ultra-fast PDF to raw pixel rendering
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <expected>

#ifdef _WIN32
  #ifdef FASTPDF2PNG_BUILD_DLL
    #define FPDF2PNG_API __declspec(dllexport)
  #else
    #define FPDF2PNG_API __declspec(dllimport)
  #endif
#else
  #define FPDF2PNG_API __attribute__((visibility("default")))
#endif

namespace fpdf2png {

enum class Error {
    Ok = 0,
    FileNotFound,
    InvalidPdf,
    RenderFailed,
    AllocFailed,
};

struct Page {
    std::unique_ptr<uint8_t[], void(*)(uint8_t*)> data;  // RGBA pixels
    int32_t width{};
    int32_t height{};
    int32_t stride{};     // bytes per row (64-byte aligned)

    // Convenience: pixel span
    [[nodiscard]] std::span<const uint8_t> pixels() const {
        return {data.get(), static_cast<size_t>(stride) * height};
    }

    // Convenience: row span
    [[nodiscard]] std::span<const uint8_t> row(int y) const {
        return {data.get() + y * stride, static_cast<size_t>(width) * 4};
    }

    Page() : data(nullptr, [](uint8_t*){}) {}
    Page(uint8_t* p, int32_t w, int32_t h, int32_t s)
        : data(p, [](uint8_t* ptr){ std::free(ptr); }), width(w), height(h), stride(s) {}
    Page(Page&&) = default;
    Page& operator=(Page&&) = default;
};

using RenderResult = std::expected<std::vector<Page>, Error>;

struct Options {
    float dpi = 150.0f;
    bool no_aa = false;    // disable anti-aliasing for speed
    int workers = 0;       // 0 = auto (number of CPU cores)
};

// RAII engine — holds PDFium initialized
class FPDF2PNG_API Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Get page count
    [[nodiscard]] int page_count(std::string_view pdf_path) const;
    [[nodiscard]] int page_count(std::span<const uint8_t> pdf_data) const;

    // Render all pages to raw RGBA
    [[nodiscard]] RenderResult render(std::string_view pdf_path, Options opts = {}) const;
    [[nodiscard]] RenderResult render(std::span<const uint8_t> pdf_data, Options opts = {}) const;

    // Render specific page range [start, end)
    [[nodiscard]] RenderResult render_pages(std::string_view pdf_path,
                                             int start, int end, Options opts = {}) const;

    // Process many PDFs in parallel using fork().
    // Forks num_workers processes; each grabs the next PDF via atomic counter.
    // opts.workers controls per-PDF page parallelism (0 = single-threaded per PDF).
    // Callback runs in worker process — use it for OCR, analysis, etc.
    // Returns total number of PDFs successfully processed.
    using PageCallback = std::function<void(std::string_view pdf_path,
                                            std::vector<Page>& pages)>;
    int process_many(const std::vector<std::string>& pdf_paths,
                     Options opts, int num_workers,
                     PageCallback callback) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Persistent worker pool — submit PDFs, get results as they complete.
// Workers stay alive until Pool is destroyed.
//
//   Pool pool(8, {.dpi = 300});
//   pool.submit("a.pdf");
//   pool.submit("b.pdf");
//   while (auto r = pool.next())       // blocks until one result ready
//       for (auto& page : r->pages)
//           ocr(page);
//
#ifndef _WIN32

struct PoolResult {
    std::string pdf_path;
    std::vector<Page> pages;
};

class FPDF2PNG_API Pool {
public:
    explicit Pool(int num_workers, Options opts = {});
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Submit a PDF. Non-blocking.
    void submit(const std::string& pdf_path);

    // Get next completed result. Blocks until one is ready.
    // Returns nullopt when all submitted work is done (after finish()).
    std::optional<PoolResult> next();

    // Signal no more submits — next() will return nullopt after last result.
    void finish();

    [[nodiscard]] int submitted() const;
    [[nodiscard]] int completed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

// C API for FFI (Python ctypes, Node ffi-napi, etc.)
extern "C" {

struct fpdf2png_page_c {
    uint8_t* data;
    int32_t width, height, stride;
};

FPDF2PNG_API void fpdf2png_init();
FPDF2PNG_API void fpdf2png_shutdown();
FPDF2PNG_API int  fpdf2png_page_count(const char* path);
FPDF2PNG_API int  fpdf2png_render(const char* path, float dpi, int no_aa,
                                   fpdf2png_page_c** out, int* count);
FPDF2PNG_API int  fpdf2png_render_mem(const uint8_t* data, size_t size,
                                       float dpi, int no_aa,
                                       fpdf2png_page_c** out, int* count);
FPDF2PNG_API void fpdf2png_free(fpdf2png_page_c* pages, int count);

} // extern "C"

} // namespace fpdf2png
