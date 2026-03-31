// fastpdf2png — Pool API (Unix fork-based parallelism)
// SPDX-License-Identifier: MIT

#pragma once

#include "fastpdf2png/export.h"
#include "fastpdf2png/types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#define FASTPDF2PNG_HAS_POOL 1

namespace fpdf2png {

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

    /// Submit a PDF for rendering. Non-blocking.
    void submit(std::string pdf_path);

    /// Retrieve the next completed result. Blocks until one is ready.
    [[nodiscard]] std::optional<PoolResult> next();

    /// Signal that no more PDFs will be submitted.
    void finish();

    [[nodiscard]] int submitted() const;
    [[nodiscard]] int completed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fpdf2png

#endif // !_WIN32
