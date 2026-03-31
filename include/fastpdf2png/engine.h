// fastpdf2png — Engine API (RAII PDFium wrapper)
// SPDX-License-Identifier: MIT

#pragma once

#include "fastpdf2png/export.h"
#include "fastpdf2png/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fpdf2png {

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
    // WARNING: callback runs in a forked child process. Mutations to parent
    // heap state (vectors, files, etc.) are invisible to the parent.
    // Use this for side-effect-only work (writing files to disk, OCR, etc.)
    // or use Pool for result collection.
    // Returns total number of PDFs successfully processed.
    using PageCallback = std::function<void(std::string_view pdf_path,
                                            std::vector<Page>& pages)>;
    [[nodiscard]] int process_many(const std::vector<std::string>& pdf_paths,
                                    Options opts, int num_workers,
                                    PageCallback callback) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fpdf2png
