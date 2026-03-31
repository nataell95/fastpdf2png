// fastpdf2png — Core types
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>
#include <expected>

namespace fpdf2png {

// ── Error codes ─────────────────────────────────────────────────────
enum class Error {
    Ok = 0,
    FileNotFound,
    InvalidPdf,
    RenderFailed,
    AllocFailed,
};

// ── Zero-overhead deleter for malloc'd pixel buffers ────────────────
struct FreeDeleter {
    void operator()(uint8_t* p) const noexcept { std::free(p); }
};

// ── Rendered page ───────────────────────────────────────────────────
struct Page {
    std::unique_ptr<uint8_t[], FreeDeleter> data;  // RGBA pixels
    int32_t width{};
    int32_t height{};
    int32_t stride{};     // bytes per row (64-byte aligned)

    [[nodiscard]] std::span<const uint8_t> pixels() const {
        return {data.get(), static_cast<size_t>(stride) * height};
    }

    [[nodiscard]] std::span<const uint8_t> row(int y) const {
        return {data.get() + y * stride, static_cast<size_t>(width) * 4};
    }

    Page() = default;
    Page(uint8_t* p, int32_t w, int32_t h, int32_t s)
        : data(p), width(w), height(h), stride(s) {}
    Page(Page&&) = default;
    Page& operator=(Page&&) = default;
};

// ── Render options ──────────────────────────────────────────────────
struct Options {
    float dpi = 150.0f;
    bool no_aa = false;    // disable anti-aliasing for speed
    int workers = 0;       // 0 = auto (number of CPU cores)
};

// ── Result type ─────────────────────────────────────────────────────
using RenderResult = std::expected<std::vector<Page>, Error>;

} // namespace fpdf2png
