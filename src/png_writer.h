// fastpdf2png - Ultra-fast PDF to PNG converter
// SPDX-License-Identifier: MIT

#ifndef FASTPDF2PNG_FAST_PNG_WRITER_H_
#define FASTPDF2PNG_FAST_PNG_WRITER_H_

#include <cstdint>
#include <cstddef>

namespace fast_png {

constexpr int kSuccess = 0;
constexpr int kErrorInvalidParams = 1;
constexpr int kErrorAllocFailed = 2;
constexpr int kErrorCompressFailed = 3;
constexpr int kErrorFileOpenFailed = 4;
constexpr int kErrorFileWriteFailed = 5;

constexpr int kCompressFast = 0;
constexpr int kCompressMedium = 1;
constexpr int kCompressBest = 2;

// Write BGRA pixel buffer to PNG file.
int WriteBgra(const char* filename,
              const uint8_t* pixels,
              int width, int height, int stride,
              int compression_level = kCompressFast);

// Write BGRA pixel buffer to PNG in memory.
int WriteBgraToMemory(const uint8_t* pixels,
                      int width, int height, int stride,
                      uint8_t** out_data, size_t* out_size,
                      int compression_level = kCompressFast);

}  // namespace fast_png

#endif  // FASTPDF2PNG_FAST_PNG_WRITER_H_
