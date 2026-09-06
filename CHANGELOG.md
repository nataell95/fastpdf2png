# Changelog

## 2.0.10

- CI: the Windows preset no longer pins the Visual Studio 2022 generator (GitHub's `windows-latest` image moved on); the newest installed Visual Studio is used.

## 2.0.9

(2.0.1 – 2.0.8 were build/CI releases of the 2.0.0 code; the `VERSION` file now matches the tags again.)

- **Embeddable build**: CMake paths are project-relative, so fastpdf2png builds as a subproject (`add_subdirectory` / `FetchContent` / `ExternalProject`) without touching the embedding project's tree.
- **`-DPDFium_DIR` is honoured before any download**: a caller that already has a PDFium SDK no longer triggers `get_pdfium.sh`; a bad path is an error instead of a silent fallback.
- **File output survives short writes**: `write(2)` returning less than requested (signals, tmpfs limits, > 2 GiB) is continued instead of reported as a failed page; `close(2)` errors are reported.
- **Forked render workers exit with `_exit`**: no double flush of the parent's stdio (in daemon mode stdout is the command pipe), no atexit handlers in the child. A failed `fork()` is logged and pages no worker produced are rendered in-process instead of failing the document.
- `get_pdfium.sh`: `TARGETARCH` (Docker buildx) and `PDFIUM_BUILD` overrides.
- New `ci-linux-arm64` preset (no `-mcpu=native`, for distributable aarch64 builds).

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
