<p align="center">
  <sub>Built for <a href="https://miruiq.com"><strong>Miruiq</strong></a> — AI-powered data extraction from PDFs and documents.</sub>
</p>

<p align="center">
  <a href="https://miruiq.com"><img src=".github/assets/miruiq_screenshot.png" alt="Miruiq" width="500"></a>
</p>

# fastpdf2png

Ultra-fast PDF to PNG converter. Pre-forked worker pool, SIMD-optimized encoding, automatic grayscale detection, zero-copy RGBA rendering. MIT licensed.

[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)]()

## Install

```bash
pip install fastpdf2png
```

Or build from source:
```bash
git clone https://github.com/nataell95/fastpdf2png.git && cd fastpdf2png
bash scripts/build.sh
```

## Usage

### CLI

```bash
# Single PDF
./build/fastpdf2png input.pdf page_%03d.png 150 8 -c 2

# Disable anti-aliasing for ~80% more throughput
./build/fastpdf2png input.pdf page_%03d.png 150 8 -c 2 --no-aa

# Streaming pool — many PDFs, max throughput
for f in docs/*.pdf; do echo "$f\toutput/${f%.pdf}_%03d.png"; done | \
  ./build/fastpdf2png --pool 150 8 -c 2

# Raw RGBA pixels to stdout (zero disk I/O, zero encoding)
# Output format: [PixelHeader (4x int32: width, height, stride, channels)] + [RGBA pixels] per page
./build/fastpdf2png --raw input.pdf 150
```

### Python

```python
import fastpdf2png

# Single file
images = fastpdf2png.to_images("doc.pdf")           # list of PIL images
files  = fastpdf2png.to_files("doc.pdf", "output/")  # save PNGs to disk
data   = fastpdf2png.to_bytes("doc.pdf")             # raw PNG bytes
raw    = fastpdf2png.to_raw("doc.pdf")               # raw RGBA pixel buffers (dicts)
n      = fastpdf2png.page_count("doc.pdf")           # page count

# Many files at once — single CLI invocation, all PDFs in parallel
fastpdf2png.batch_to_files(pdf_list, "output/", dpi=150, workers=8)

# High-throughput streaming — pre-forked worker pool
with fastpdf2png.Engine(workers=8) as eng:
    eng.to_files_many(pdf_list, "output/", dpi=150)

    # Also works one at a time
    eng.to_files("single.pdf", "output/")
    imgs = eng.to_images("single.pdf", dpi=72)
    data = eng.to_bytes("single.pdf")
    n    = eng.page_count("single.pdf")
```

#### Native C library binding (zero-copy)

```python
from fastpdf2png.native import render, render_many, page_count

# Single PDF — raw RGBA pixel buffers, no PNG encoding
pages = render("doc.pdf", dpi=150, no_aa=False)
for page in pages:
    print(page.width, page.height, page.stride, page.channels)  # 4 = RGBA
    arr = page.to_numpy()   # (H, W, 4) RGBA numpy array
    img = page.to_pil()     # PIL Image (RGB, alpha dropped)
    raw = page.data         # raw bytes (stride * height)

# Many PDFs in parallel via ProcessPoolExecutor
results = render_many(pdf_paths, dpi=150, no_aa=False, workers=8)
# results[i] = list of PageBuffer for pdf_paths[i]

# Page count without rendering
n = page_count("doc.pdf")
```

### Node.js

```js
const pdf = require("fastpdf2png");

pdf.toFiles("doc.pdf", "output/", { dpi: 150 });
const buffers = pdf.toBuffers("doc.pdf");
const count = pdf.pageCount("doc.pdf");

// Persistent engine — keeps PDFium loaded between calls
const engine = new pdf.Engine();
await engine.toFiles("doc.pdf", "output/");
const bufs = engine.toBuffers("doc.pdf");
engine.close();
```

### C++ native library

```cpp
#include "fastpdf2png/lib.h"

fpdf2png::Engine engine;

// Render all pages to raw RGBA pixels
fpdf2png::Options opts{.dpi = 150, .no_aa = false, .workers = 0};
auto result = engine.render("doc.pdf", opts);
if (result) {
    for (auto& page : *result) {
        // page.data.get()  — raw RGBA pixels (unique_ptr, auto-freed)
        // page.width, page.height, page.stride (64-byte aligned)
        auto pixels = page.pixels();   // std::span<const uint8_t> of full buffer
        auto row0   = page.row(0);     // std::span<const uint8_t> of row 0
    }
} else {
    // result.error() returns fpdf2png::Error enum:
    //   FileNotFound, InvalidPdf, RenderFailed, AllocFailed
}

// Render from in-memory PDF data (zero disk I/O)
std::vector<uint8_t> pdf_data = load_file("doc.pdf");
auto result = engine.render({pdf_data.data(), pdf_data.size()});

// Render specific page range [start, end)
auto result = engine.render_pages("doc.pdf", 0, 10, opts);

// Get page count without rendering (from file or memory)
int n = engine.page_count("doc.pdf");
int m = engine.page_count({pdf_data.data(), pdf_data.size()});
```

### C API (for FFI — Python ctypes, Node ffi-napi, etc.)

```c
#include "fastpdf2png/lib.h"

// Initialize PDFium (call once)
fpdf2png_init();

// Get page count
int n = fpdf2png_page_count("doc.pdf");

// Render all pages — returns array of {data, width, height, stride}
fpdf2png_page_c* pages = NULL;
int count = 0;
int err = fpdf2png_render("doc.pdf", 150.0f, /*no_aa=*/0, &pages, &count);
if (err == 0) {
    for (int i = 0; i < count; i++) {
        // pages[i].data   — RGBA pixels (width * 4 bytes per row, stride-aligned)
        // pages[i].width, pages[i].height, pages[i].stride
    }
    fpdf2png_free(pages, count);  // free all page buffers + array
}

// Render from memory buffer
fpdf2png_page_c* pages2 = NULL;
int count2 = 0;
fpdf2png_render_mem(pdf_bytes, pdf_size, 150.0f, 0, &pages2, &count2);
fpdf2png_free(pages2, count2);

// Shutdown PDFium (call once at exit)
fpdf2png_shutdown();
```

## Performance

Single 103-page text PDF (150 DPI, compression level 2, Apple M-series):

| Workers | AA on | AA off |
|---------|-------|--------|
| 1 | 165 | 170 |
| 2 | 317 | 313 |
| 4 | 546 | 558 |
| 8 | **931** | **881** |

Streaming pool mode — 200 separate PDFs (324 total pages, 8 workers):

| DPI | AA on | AA off |
|-----|-------|--------|
| 72 | 446 | **537** |
| 150 | 189 | **334** |
| 300 | 119 | 121 |

Same PDF repeated 200 times via pool (20,600 pages, 8 workers, 150 DPI): **1,051 pages/sec** — workers cache the document, no re-parsing overhead.

Speed depends on page content complexity. Simple text pages render at ~165 pg/s per core; pages with images and graphics render at 15-28 pg/s per core.

Smaller output files for grayscale pages thanks to automatic grayscale detection.

## How it works

<p align="center">
  <img src=".github/assets/architecture.svg" alt="Architecture" width="800">
</p>

### Rendering

Google's [PDFium](https://pdfium.googlesource.com/pdfium/) (the engine inside Chromium) renders each page into a raw RGBA bitmap using `FPDF_REVERSE_BYTE_ORDER`. This produces RGBA pixels directly — no BGRA-to-RGBA swizzle needed, and fpng can encode them with zero conversion overhead.

### Grayscale detection

Before encoding, a SIMD-accelerated pass scans every pixel to check if R == G == B. Grayscale pages are encoded as 8-bit PNG instead of 24-bit RGB, cutting data size by 66% with zero quality loss. On ARM this uses NEON `vld4/vceq` intrinsics; on x86 it uses SSE/AVX2.

### PNG encoding

Instead of the standard zlib/libpng pipeline, we use [libdeflate](https://github.com/ebiggers/libdeflate) for compression and [fpng](https://github.com/richgel999/fpng) for fast encoding. The compressed data goes directly into a pre-allocated output buffer — the PNG header, IDAT chunk, and IEND trailer are assembled around it with zero intermediate copies. CRC32 checksums use hardware-accelerated instructions (CRC32 on ARM, PCLMUL on x86). Each page is written with a single `write()` syscall.

### Pool mode

The `--pool` command pre-forks N worker processes at startup. Workers stay alive and wait for jobs on pipes (zero CPU waste when idle). The parent reads PDF paths from stdin and dispatches them immediately to workers via pipe IPC. Large multi-page PDFs are automatically split into page ranges across workers for load balancing. Each worker caches the last-opened document to avoid re-parsing for split page ranges.

On Windows, pool mode uses `CreateProcess` with anonymous pipes — same architecture, Win32 APIs.

### Memory pools

Each worker maintains process-local memory pools for pixel buffers and compression scratch space. After the first page warms up the pools, subsequent pages require zero `malloc`/`free` calls in the hot path.

## CLI reference

```
fastpdf2png <input.pdf> <output_%03d.png> [dpi] [workers] [-c level] [--no-aa]
fastpdf2png --pool [dpi] [workers] [-c level] [--no-aa]    < job_list
fastpdf2png --raw <input.pdf> [dpi] [--no-aa]
fastpdf2png --info <input.pdf>
fastpdf2png --daemon
```

| Flag | Default | Description |
|------|---------|-------------|
| `dpi` | 150 | Output resolution |
| `workers` | 4 | Parallel processes |
| `-c -1` | | Raw PPM/PGM output (no compression, max speed) |
| `-c 0` | | Fast PNG (fpng) |
| `-c 1` | | Medium PNG (fpng slower) |
| `-c 2` | 2 | Best PNG (libdeflate, smallest files) |
| `--no-aa` | | Disable anti-aliasing (faster rendering) |
| `--pool` | | Streaming worker pool (reads jobs from stdin) |
| `--raw` | | Output raw RGBA pixels to stdout (no encoding) |
| `--info` | | Print page count |
| `--daemon` | | Persistent mode (stdin commands) |

`--pool` (alias: `--batch`) reads `pdf_path\toutput_pattern` lines from stdin, one per line.

`--raw` output format per page: 16-byte header (`int32 width, height, stride, channels`) followed by `stride * height` bytes of RGBA pixel data.

## Platforms

| OS | Arch | SIMD | Pool mode |
|----|------|------|-----------|
| macOS | arm64 | NEON | fork + pipes |
| macOS | x86_64 | AVX2, SSE4.1 | fork + pipes |
| Linux | x86_64 | AVX2, SSE4.1 | fork + pipes |
| Linux | arm64 | NEON | fork + pipes |
| Windows | x86_64 | AVX2, SSE4.1 | CreateProcess + pipes |

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
