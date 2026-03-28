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

# Batch / streaming pool (many PDFs, max throughput)
for f in docs/*.pdf; do echo "$f\toutput/${f%.pdf}_%03d.png"; done | \
  ./build/fastpdf2png --pool 150 8 -c 2
```

### Python

```python
import fastpdf2png

images = fastpdf2png.to_images("doc.pdf")        # list of PIL images
fastpdf2png.to_files("doc.pdf", "output/")        # save PNGs to disk
data   = fastpdf2png.to_bytes("doc.pdf")          # raw PNG bytes
n      = fastpdf2png.page_count("doc.pdf")        # page count

# High-throughput batch processing
with fastpdf2png.Engine(workers=8) as pdf:
    pdf.to_files_many(pdf_list, "output/", dpi=150)
```

### Node.js

```js
const pdf = require("fastpdf2png");

pdf.toFiles("doc.pdf", "output/", { dpi: 150 });
const buffers = pdf.toBuffers("doc.pdf");
const count = pdf.pageCount("doc.pdf");

// Batch processing
const engine = new pdf.Engine();
await engine.toFiles("doc.pdf", "output/");
engine.close();
```

## Performance

Worker scaling on a single 103-page PDF (150 DPI, compression level 2, Apple M-series):

| Workers | Pages/sec |
|---------|-----------|
| 1 | 141 |
| 2 | 309 |
| 4 | 555 |
| 8 | **894** |

Streaming pool mode — 200 separate PDFs (324 total pages, 150 DPI, 8 workers):

| DPI | Pages/sec |
|-----|-----------|
| 72 | **574** |
| 150 | **316** |
| 300 | 101 |

Smaller output files for grayscale pages thanks to automatic grayscale detection.

## How it works

<p align="center">
  <img src=".github/assets/architecture.svg" alt="Architecture" width="800">
</p>

### Rendering

Google's [PDFium](https://pdfium.googlesource.com/pdfium/) (the engine inside Chromium) renders each page into a raw RGBA bitmap using `FPDF_REVERSE_BYTE_ORDER`. This produces RGBA pixels directly — no BGRA-to-RGBA swizzle needed, and fpng can encode them with zero conversion overhead.

### Grayscale detection

Before encoding, a SIMD-accelerated pass scans every pixel to check if R == G == B. Most document pages (text, tables, charts) are grayscale — detecting this lets us encode them as 8-bit PNG instead of 24-bit RGB, cutting data size by 66% with zero quality loss. On ARM this uses NEON `vld4/vceq` intrinsics; on x86 it uses SSE/AVX2.

### PNG encoding

Instead of the standard zlib/libpng pipeline, we use [libdeflate](https://github.com/ebiggers/libdeflate) for compression and [fpng](https://github.com/richgel999/fpng) for fast encoding. The compressed data goes directly into a pre-allocated output buffer — the PNG header, IDAT chunk, and IEND trailer are assembled around it with zero intermediate copies. CRC32 checksums use hardware-accelerated instructions (CRC32 on ARM, PCLMUL on x86). Each page is written with a single `write()` syscall.

### Pool mode

The `--pool` command pre-forks N worker processes at startup. Workers stay alive and wait for jobs on pipes (zero CPU waste when idle). The parent reads PDF paths from stdin and dispatches them immediately to workers via pipe IPC. Large multi-page PDFs are automatically split into page ranges across workers for load balancing. Each worker loads PDFs into memory with `read()` and parses them with `FPDF_LoadMemDocument64`, eliminating syscalls during PDF parsing.

On Windows, pool mode uses `CreateProcess` with anonymous pipes — same architecture, Win32 APIs.

### Memory pools

Each worker maintains process-local memory pools for pixel buffers and compression scratch space. After the first page warms up the pools, subsequent pages require zero `malloc`/`free` calls in the hot path.

## CLI reference

```
fastpdf2png <input.pdf> <output_%03d.png> [dpi] [workers] [-c level]
fastpdf2png --pool [dpi] [workers] [-c level]    < job_list
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
| `--pool` | | Streaming worker pool (reads jobs from stdin) |
| `--info` | | Print page count |
| `--daemon` | | Persistent mode (stdin commands) |

Pool mode reads `pdf_path\toutput_pattern` lines from stdin, one per line.

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
