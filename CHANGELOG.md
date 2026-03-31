# Changelog

## 2.0.0

- **Codebase restructured**: CMake build system (with presets), public headers moved to `include/fastpdf2png/`, source split into `src/{cli,lib,internal,png}`, language bindings moved to `bindings/{python,node}`, vendored deps in `third_party/`, `VERSION` file as single source of truth for versioning.
- **Pool mode** (`--pool`/`--batch`): pre-forked worker pool with pipe-based IPC for streaming high-throughput batch processing. Workers stay alive between PDFs — zero fork overhead per file.
- **Windows pool mode**: `CreateProcess` + anonymous pipes, same architecture as Unix.
- **FPDF_REVERSE_BYTE_ORDER**: PDFium renders directly to RGBA — eliminates BGRA→RGBA swizzle pass. fpng encodes with zero conversion overhead.
- **FPDF_LoadMemDocument64**: PDFs loaded from memory buffers, eliminating syscalls during PDF parsing.
- **Render optimizations**: disabled image/path/text anti-aliasing for ~30-40% faster rendering at 150 DPI.
- **Large PDF page splitting**: multi-page PDFs automatically split into page ranges across workers for load balancing.
- **Raw PPM/PGM output** (`-c -1`): zero-compression mode for GPU pipelines and OCR — max throughput.
- **RGBA-native PNG encoding**: new `WriteRgba` path in png_writer — NEON/SSE optimized RGBA→RGB conversion (just drop alpha, no swap).
- **Pipe safety**: `ReadFull`/`WriteFull` helpers handle short reads/writes. `drain_results()` prevents pipe buffer deadlock.
- **Engine.to_files_many()**: Python API for batch processing, uses `--pool` internally.
- **batch_to_files()**: standalone function for batch conversion.
- Verified: 0 memory leaks (macOS `leaks`), 0 ASan errors, 0 UBSan errors.
- Performance: 537 pg/s at 120 DPI, 833 pg/s at 72 DPI (14 workers, Apple M-series).

## 1.0.0

- Initial release
- PDFium-based rendering with SIMD-optimized PNG encoding (NEON, AVX2, SSE)
- Auto grayscale detection for smaller output files
- fork()-based parallelism with shared-memory work stealing
- Python SDK: `to_images()`, `to_files()`, `to_bytes()`, `page_count()`, `Engine`
- Node.js SDK: `toFiles()`, `toBuffers()`, `pageCount()`, `Engine`
- CLI with daemon mode for batch processing
- CI/CD: builds for macOS (arm64, x86_64), Linux (x86_64, arm64)
- Publishes to PyPI and npm on tag
