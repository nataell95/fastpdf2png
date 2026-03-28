// fastpdf2png - Ultra-fast PNG encoder
// SPDX-License-Identifier: MIT

#include "png_writer.h"
#include "memory_pool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "libdeflate.h"
#include "fpng/fpng.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define USE_NEON 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(__AVX2__)
#define USE_AVX2 1
#endif
#define USE_SSE 1
#endif

namespace {

// ---------------------------------------------------------------------------
// Init + per-process state (no thread_local — each fork'd worker is single-threaded)
// ---------------------------------------------------------------------------

void EnsureFpngInit() {
  static std::once_flag flag;
  std::call_once(flag, fpng::fpng_init);
}

libdeflate_compressor* GetCompressor() {
  static thread_local libdeflate_compressor* comp = nullptr;
  if (!comp)
    comp = libdeflate_alloc_compressor(1);
  return comp;
}

struct ReusableBuffers {
  uint8_t* raw = nullptr;
  uint8_t* comp = nullptr;
  size_t raw_cap = 0;
  size_t comp_cap = 0;

  uint8_t* AcquireRaw(size_t size) {
    if (size > raw_cap) {
      std::free(raw);
      raw_cap = size + size / 4;
      raw = static_cast<uint8_t*>(std::malloc(raw_cap));
    }
    return raw;
  }

  uint8_t* AcquireComp(size_t size) {
    if (size > comp_cap) {
      std::free(comp);
      comp_cap = size + size / 4;
      comp = static_cast<uint8_t*>(std::malloc(comp_cap));
    }
    return comp;
  }
};

ReusableBuffers& GetBuffers() {
  static thread_local ReusableBuffers bufs;
  return bufs;
}

// ---------------------------------------------------------------------------
// BGRA → RGBA (fpng path only)
// ---------------------------------------------------------------------------

#if USE_AVX2
void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height, int src_stride) {
  static const auto shuf = _mm256_setr_epi8(
      2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
      2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    auto* s = src + y * src_stride;
    auto* d = dst + y * dst_stride;
    int x = 0;
    for (; x + 8 <= width; x += 8) {
      auto p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + x * 4));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + x * 4), _mm256_shuffle_epi8(p, shuf));
    }
    const auto sse_shuf = _mm_setr_epi8(2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
    for (; x + 4 <= width; x += 4) {
      auto p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x * 4));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x * 4), _mm_shuffle_epi8(p, sse_shuf));
    }
    for (; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#elif USE_SSE
void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height, int src_stride) {
  static const auto shuf = _mm_setr_epi8(2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15);
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    auto* s = src + y * src_stride;
    auto* d = dst + y * dst_stride;
    int x = 0;
    for (; x + 4 <= width; x += 4) {
      auto p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x * 4));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x * 4), _mm_shuffle_epi8(p, shuf));
    }
    for (; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#else
void BgraToRgba(const uint8_t* src, uint8_t* dst, int width, int height, int src_stride) {
  const int dst_stride = width * 4;
  for (int y = 0; y < height; y++) {
    auto* s = src + y * src_stride;
    auto* d = dst + y * dst_stride;
    for (int x = 0; x < width; x++) {
      d[x*4] = s[x*4+2]; d[x*4+1] = s[x*4+1]; d[x*4+2] = s[x*4]; d[x*4+3] = s[x*4+3];
    }
  }
}
#endif

// ---------------------------------------------------------------------------
// RGBA → RGB row conversion (just drop alpha, no swap needed)
// ---------------------------------------------------------------------------

inline void ConvertRowRgbaToRgb(uint8_t* dst, const uint8_t* src, int width) {
  int x = 0;
#if USE_NEON
  for (; x + 16 <= width; x += 16) {
    auto rgba = vld4q_u8(src + x * 4);
    uint8x16x3_t rgb = { rgba.val[0], rgba.val[1], rgba.val[2] };
    vst3q_u8(dst + x * 3, rgb);
  }
#elif USE_SSE
  const auto shuf = _mm_setr_epi8(0,1,2, 4,5,6, 8,9,10, 12,13,14, -1,-1,-1,-1);
  for (; x + 4 <= width; x += 4) {
    auto p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
    auto rgb = _mm_shuffle_epi8(p, shuf);
    uint8_t tmp[16];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), rgb);
    std::memcpy(dst + x * 3, tmp, 12);
  }
#endif
  for (; x < width; x++) {
    dst[x*3] = src[x*4]; dst[x*3+1] = src[x*4+1]; dst[x*3+2] = src[x*4+2];
  }
}

// BGRA → RGB (legacy, with swap)
inline void ConvertRowBgraToRgb(uint8_t* dst, const uint8_t* src, int width) {
  int x = 0;
#if USE_NEON
  for (; x + 16 <= width; x += 16) {
    auto bgra = vld4q_u8(src + x * 4);
    uint8x16x3_t rgb = { bgra.val[2], bgra.val[1], bgra.val[0] };
    vst3q_u8(dst + x * 3, rgb);
  }
#elif USE_SSE
  const auto shuf = _mm_setr_epi8(2,1,0, 6,5,4, 10,9,8, 14,13,12, -1,-1,-1,-1);
  for (; x + 4 <= width; x += 4) {
    auto p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
    auto rgb = _mm_shuffle_epi8(p, shuf);
    uint8_t tmp[16];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), rgb);
    std::memcpy(dst + x * 3, tmp, 12);
  }
#endif
  for (; x < width; x++) {
    dst[x*3] = src[x*4+2]; dst[x*3+1] = src[x*4+1]; dst[x*3+2] = src[x*4];
  }
}

// ---------------------------------------------------------------------------
// Grayscale detection
// ---------------------------------------------------------------------------

inline bool IsRowGrayscale(const uint8_t* src, int width) {
  int x = 0;
#if USE_NEON
  for (; x + 16 <= width; x += 16) {
    auto bgra = vld4q_u8(src + x * 4);
    auto eq = vandq_u8(vceqq_u8(bgra.val[0], bgra.val[1]),
                       vceqq_u8(bgra.val[1], bgra.val[2]));
    if (vminvq_u8(eq) != 0xFF) return false;
  }
#endif
  for (; x < width; x++) {
    if (src[x*4] != src[x*4+1] || src[x*4+1] != src[x*4+2])
      return false;
  }
  return true;
}

bool IsImageGrayscale(const uint8_t* pixels, int width, int height, int stride) {
  if (!IsRowGrayscale(pixels, width)) return false;
  if (!IsRowGrayscale(pixels + (height / 2) * stride, width)) return false;
  if (!IsRowGrayscale(pixels + (height - 1) * stride, width)) return false;
  for (int y = 1; y < height - 1; y++) {
    if (!IsRowGrayscale(pixels + y * stride, width))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Raw PNG data preparation
// ---------------------------------------------------------------------------

// RGBA input: grayscale = take R channel (byte 0)
void PrepareGrayscaleRgba(uint8_t* raw, const uint8_t* pixels, int width, int height, int stride) {
  const auto row_bytes = 1 + static_cast<size_t>(width);
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;
    auto* dst = raw + y * row_bytes + 1;
    auto* src = pixels + y * stride;
    int x = 0;
#if USE_NEON
    for (; x + 16 <= width; x += 16) {
      auto rgba = vld4q_u8(src + x * 4);
      vst1q_u8(dst + x, rgba.val[0]);
    }
#endif
    for (; x < width; x++)
      dst[x] = src[x * 4];
  }
}

// RGBA input: RGB = just drop alpha
void PrepareRgbFromRgba(uint8_t* raw, const uint8_t* pixels, int width, int height, int stride) {
  const auto row_bytes = 1 + static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;
    ConvertRowRgbaToRgb(raw + y * row_bytes + 1, pixels + y * stride, width);
  }
}

// Legacy BGRA versions
void PrepareGrayscale(uint8_t* raw, const uint8_t* pixels, int width, int height, int stride) {
  const auto row_bytes = 1 + static_cast<size_t>(width);
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;
    auto* dst = raw + y * row_bytes + 1;
    auto* src = pixels + y * stride;
    int x = 0;
#if USE_NEON
    for (; x + 16 <= width; x += 16) {
      auto bgra = vld4q_u8(src + x * 4);
      vst1q_u8(dst + x, bgra.val[0]);
    }
#endif
    for (; x < width; x++)
      dst[x] = src[x * 4];
  }
}

void PrepareRgb(uint8_t* raw, const uint8_t* pixels, int width, int height, int stride) {
  const auto row_bytes = 1 + static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; y++) {
    raw[y * row_bytes] = 0;
    ConvertRowBgraToRgb(raw + y * row_bytes + 1, pixels + y * stride, width);
  }
}

// ---------------------------------------------------------------------------
// PNG assembly
// ---------------------------------------------------------------------------

constexpr uint8_t kPngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
constexpr size_t kHeaderSize = 41;
constexpr size_t kTrailerSize = 16;

inline void WriteU32Be(uint8_t* p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v & 0xFF;
}

size_t AssemblePng(uint8_t* buf, int width, int height, size_t compressed_size, int color_type) {
  auto* p = buf;
  std::memcpy(p, kPngSignature, 8); p += 8;

  // IHDR
  WriteU32Be(p, 13);
  std::memcpy(p + 4, "IHDR", 4);
  WriteU32Be(p + 8, width);
  WriteU32Be(p + 12, height);
  p[16] = 8; p[17] = color_type; p[18] = 0; p[19] = 0; p[20] = 0;
  WriteU32Be(p + 21, libdeflate_crc32(0, p + 4, 17));
  p += 25;

  // IDAT
  WriteU32Be(p, static_cast<uint32_t>(compressed_size));
  std::memcpy(p + 4, "IDAT", 4);
  WriteU32Be(p + 8 + compressed_size, libdeflate_crc32(0, p + 4, 4 + compressed_size));
  p += 8 + compressed_size + 4;

  // IEND
  constexpr uint8_t kIend[12] = {0,0,0,0, 'I','E','N','D', 0xAE,0x42,0x60,0x82};
  std::memcpy(p, kIend, 12);
  p += 12;

  return static_cast<size_t>(p - buf);
}

// ---------------------------------------------------------------------------
// Write helpers
// ---------------------------------------------------------------------------

int WriteFile(const char* filename, const uint8_t* data, size_t size) {
#ifdef _WIN32
  int fd = _open(filename, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
  if (fd < 0) return fast_png::kErrorFileOpenFailed;
  auto written = _write(fd, data, static_cast<unsigned int>(size));
  _close(fd);
  return (written == static_cast<int>(size)) ? fast_png::kSuccess : fast_png::kErrorFileWriteFailed;
#else
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return fast_png::kErrorFileOpenFailed;
#ifdef __APPLE__
  fcntl(fd, F_NOCACHE, 1);
#endif
  auto written = write(fd, data, size);
  close(fd);
  return (written == static_cast<ssize_t>(size)) ? fast_png::kSuccess : fast_png::kErrorFileWriteFailed;
#endif
}

// ---------------------------------------------------------------------------
// Core encode paths
// ---------------------------------------------------------------------------

int EncodeLibdeflate(const char* filename, const uint8_t* pixels,
                     int width, int height, int stride) {
  const bool gray = IsImageGrayscale(pixels, width, height, stride);
  const int color_type = gray ? 0 : 2;
  const size_t row_bytes = 1 + static_cast<size_t>(width) * (gray ? 1 : 3);
  const size_t raw_size = row_bytes * height;

  auto& bufs = GetBuffers();
  auto* raw = bufs.AcquireRaw(raw_size);
  if (!raw) return fast_png::kErrorAllocFailed;

  if (gray) PrepareGrayscale(raw, pixels, width, height, stride);
  else      PrepareRgb(raw, pixels, width, height, stride);

  auto* c = GetCompressor();
  if (!c) return fast_png::kErrorAllocFailed;

  const size_t bound = libdeflate_zlib_compress_bound(c, raw_size);
  auto* out = bufs.AcquireComp(kHeaderSize + bound + kTrailerSize);
  if (!out) return fast_png::kErrorAllocFailed;

  const size_t comp = libdeflate_zlib_compress(c, raw, raw_size, out + kHeaderSize, bound);
  if (comp == 0) return fast_png::kErrorCompressFailed;

  const size_t png_size = AssemblePng(out, width, height, comp, color_type);
  return WriteFile(filename, out, png_size);
}

int EncodeLibdeflateToMemory(const uint8_t* pixels, int width, int height,
                             int stride, uint8_t** out_data, size_t* out_size) {
  const bool gray = IsImageGrayscale(pixels, width, height, stride);
  const int color_type = gray ? 0 : 2;
  const size_t row_bytes = 1 + static_cast<size_t>(width) * (gray ? 1 : 3);
  const size_t raw_size = row_bytes * height;

  auto& bufs = GetBuffers();
  auto* raw = bufs.AcquireRaw(raw_size);
  if (!raw) return fast_png::kErrorAllocFailed;

  if (gray) PrepareGrayscale(raw, pixels, width, height, stride);
  else      PrepareRgb(raw, pixels, width, height, stride);

  auto* c = GetCompressor();
  if (!c) return fast_png::kErrorAllocFailed;

  const size_t bound = libdeflate_zlib_compress_bound(c, raw_size);
  const size_t total = kHeaderSize + bound + kTrailerSize;
  *out_data = static_cast<uint8_t*>(std::malloc(total));
  if (!*out_data) { *out_size = 0; return fast_png::kErrorAllocFailed; }

  const size_t comp = libdeflate_zlib_compress(c, raw, raw_size, *out_data + kHeaderSize, bound);
  if (comp == 0) { std::free(*out_data); *out_data = nullptr; *out_size = 0; return fast_png::kErrorCompressFailed; }

  *out_size = AssemblePng(*out_data, width, height, comp, color_type);
  return fast_png::kSuccess;
}

// ---------------------------------------------------------------------------
// Raw PPM/PGM output (zero compression, max throughput)
// Grayscale → PGM (P5), Color → PPM (P6)
// ---------------------------------------------------------------------------

int WriteRawPpm(const char* filename, const uint8_t* pixels,
                int width, int height, int stride) {
  const bool gray = IsImageGrayscale(pixels, width, height, stride);

  // Build header
  char header[64];
  int hlen;
  if (gray)
    hlen = std::snprintf(header, sizeof(header), "P5\n%d %d\n255\n", width, height);
  else
    hlen = std::snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height);

  const size_t pixel_bytes = gray
      ? static_cast<size_t>(width) * height
      : static_cast<size_t>(width) * height * 3;
  const size_t total = hlen + pixel_bytes;

  auto& bufs = GetBuffers();
  auto* out = bufs.AcquireRaw(total);
  if (!out) return fast_png::kErrorAllocFailed;

  std::memcpy(out, header, hlen);
  auto* dst = out + hlen;

  if (gray) {
    for (int y = 0; y < height; y++) {
      auto* src = pixels + y * stride;
      auto* row = dst + y * width;
      int x = 0;
#if USE_NEON
      for (; x + 16 <= width; x += 16) {
        auto rgba = vld4q_u8(src + x * 4);
        vst1q_u8(row + x, rgba.val[0]);
      }
#endif
      for (; x < width; x++)
        row[x] = src[x * 4];
    }
  } else {
    for (int y = 0; y < height; y++)
      ConvertRowRgbaToRgb(dst + y * width * 3, pixels + y * stride, width);
  }

  return WriteFile(filename, out, total);
}

// RGBA-native libdeflate encode (no BGRA→RGBA swizzle needed)
int EncodeLibdeflateRgba(const char* filename, const uint8_t* pixels,
                          int width, int height, int stride) {
  const bool gray = IsImageGrayscale(pixels, width, height, stride);
  const int color_type = gray ? 0 : 2;
  const size_t row_bytes = 1 + static_cast<size_t>(width) * (gray ? 1 : 3);
  const size_t raw_size = row_bytes * height;

  auto& bufs = GetBuffers();
  auto* raw = bufs.AcquireRaw(raw_size);
  if (!raw) return fast_png::kErrorAllocFailed;

  if (gray) PrepareGrayscaleRgba(raw, pixels, width, height, stride);
  else      PrepareRgbFromRgba(raw, pixels, width, height, stride);

  auto* c = GetCompressor();
  if (!c) return fast_png::kErrorAllocFailed;

  const size_t bound = libdeflate_zlib_compress_bound(c, raw_size);
  auto* out = bufs.AcquireComp(kHeaderSize + bound + kTrailerSize);
  if (!out) return fast_png::kErrorAllocFailed;

  const size_t comp = libdeflate_zlib_compress(c, raw, raw_size, out + kHeaderSize, bound);
  if (comp == 0) return fast_png::kErrorCompressFailed;

  const size_t png_size = AssemblePng(out, width, height, comp, color_type);
  return WriteFile(filename, out, png_size);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace fast_png {

int WriteRgba(const char* filename, const uint8_t* pixels,
              int width, int height, int stride, int compression_level) {
  if (!filename || !pixels || width <= 0 || height <= 0)
    return kErrorInvalidParams;

  if (compression_level == kCompressRaw)
    return WriteRawPpm(filename, pixels, width, height, stride);

  if (compression_level == kCompressBest)
    return EncodeLibdeflateRgba(filename, pixels, width, height, stride);

  // fpng natively accepts RGBA — feed directly, NO conversion needed
  EnsureFpngInit();
  int flags = (compression_level >= kCompressMedium) ? fpng::FPNG_ENCODE_SLOWER : 0;
  return fpng::fpng_encode_image_to_file(filename, pixels, width, height, 4, flags)
             ? kSuccess : kErrorCompressFailed;
}

int WriteRgbaToMemory(const uint8_t* pixels, int width, int height,
                      int stride, uint8_t** out_data, size_t* out_size,
                      int compression_level) {
  if (!pixels || width <= 0 || height <= 0 || !out_data || !out_size)
    return kErrorInvalidParams;

  // For kCompressBest, use libdeflate with RGBA-native path
  // For fpng: feed RGBA directly, no conversion
  EnsureFpngInit();
  int flags = (compression_level >= kCompressMedium) ? fpng::FPNG_ENCODE_SLOWER : 0;
  std::vector<uint8_t> buf;
  if (!fpng::fpng_encode_image_to_memory(pixels, width, height, 4, buf, flags))
    return kErrorCompressFailed;

  *out_size = buf.size();
  *out_data = static_cast<uint8_t*>(std::malloc(*out_size));
  if (!*out_data) { *out_size = 0; return kErrorAllocFailed; }
  std::memcpy(*out_data, buf.data(), *out_size);
  return kSuccess;
}

int WriteBgra(const char* filename, const uint8_t* pixels,
              int width, int height, int stride, int compression_level) {
  if (!filename || !pixels || width <= 0 || height <= 0)
    return kErrorInvalidParams;

  if (compression_level == kCompressBest)
    return EncodeLibdeflate(filename, pixels, width, height, stride);

  EnsureFpngInit();
  auto& pool = GetProcessLocalPool();
  auto* rgba = pool.Acquire(static_cast<size_t>(width) * height * 4);
  if (!rgba) return kErrorAllocFailed;

  BgraToRgba(pixels, rgba, width, height, stride);

  int flags = (compression_level >= kCompressMedium) ? fpng::FPNG_ENCODE_SLOWER : 0;
  return fpng::fpng_encode_image_to_file(filename, rgba, width, height, 4, flags)
             ? kSuccess : kErrorCompressFailed;
}

int WriteBgraToMemory(const uint8_t* pixels, int width, int height,
                      int stride, uint8_t** out_data, size_t* out_size,
                      int compression_level) {
  if (!pixels || width <= 0 || height <= 0 || !out_data || !out_size)
    return kErrorInvalidParams;

  if (compression_level == kCompressBest)
    return EncodeLibdeflateToMemory(pixels, width, height, stride, out_data, out_size);

  EnsureFpngInit();
  auto& pool = GetProcessLocalPool();
  auto* rgba = pool.Acquire(static_cast<size_t>(width) * height * 4);
  if (!rgba) return kErrorAllocFailed;

  BgraToRgba(pixels, rgba, width, height, stride);

  int flags = (compression_level >= kCompressMedium) ? fpng::FPNG_ENCODE_SLOWER : 0;
  std::vector<uint8_t> buf;
  if (!fpng::fpng_encode_image_to_memory(rgba, width, height, 4, buf, flags))
    return kErrorCompressFailed;

  *out_size = buf.size();
  *out_data = static_cast<uint8_t*>(std::malloc(*out_size));
  if (!*out_data) { *out_size = 0; return kErrorAllocFailed; }
  std::memcpy(*out_data, buf.data(), *out_size);
  return kSuccess;
}

}  // namespace fast_png
