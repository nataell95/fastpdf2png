# AI Agent Instructions — fastpdf2png

Ultra-fast PDF to PNG converter. 49x faster than MuPDF. MIT licensed.

## Structure

```
fastpdf2png/
├── CMakeLists.txt           CMake build (single source of truth)
├── CMakePresets.json         release / debug / asan presets
├── VERSION                   semver, read by CMake + Python
├── cmake/                    DetectSIMD, FindPDFium, Sanitizers
├── include/fastpdf2png/      Public C/C++ headers
│   ├── fastpdf2png.h          Main include (includes all below)
│   ├── export.h               DLL export macros
│   ├── types.h                Enums and structs
│   ├── engine.h               C++ Engine class
│   ├── pool.h                 C++ Pool class
│   └── c_api.h                C API for FFI
├── src/
│   ├── cli/                   CLI entry point + subcommands
│   ├── internal/              pdfium_render, file_io, shared_memory
│   ├── lib/                   engine.cpp, pool.cpp, c_api.cpp
│   └── png/                   png_writer + memory_pool
├── third_party/               fpng, libdeflate, stb (vendored)
├── tests/
│   ├── unit/                  C++ unit tests (CTest)
│   ├── integration/           C++ integration tests (CTest)
│   └── python/                pytest-based Python binding tests
├── benchmarks/                C++ benchmarks
├── bindings/
│   ├── python/                Python package (pip install -e .)
│   └── node/                  Node.js bindings
├── scripts/                   build.sh, get_pdfium.sh, benchmark.sh
└── .github/workflows/         CI/CD
```

`pdfium/` and `build/` are gitignored (downloaded/generated at build time).

## Build

```bash
# Quick build (release):
cmake --preset release && cmake --build build

# Debug + tests:
cmake --preset debug && cmake --build build/debug && ctest --test-dir build/debug

# Legacy wrapper (calls cmake internally):
bash scripts/build.sh
```

## Test

```bash
# C++ tests (unit + integration):
cmake --preset debug && cmake --build build/debug && ctest --test-dir build/debug

# Python binding tests:
FASTPDF2PNG_TEST_PDF=doc.pdf pytest tests/python/
```

## API

```python
import fastpdf2png

fastpdf2png.to_images("doc.pdf")           # → [PIL.Image, ...]
fastpdf2png.to_files("doc.pdf", "out/")    # → [Path, ...]
fastpdf2png.to_bytes("doc.pdf")            # → [bytes, ...]
fastpdf2png.page_count("doc.pdf")          # → int

with fastpdf2png.Engine() as pdf:          # persistent mode
    pdf.to_images("doc.pdf")
```

```c
// C API (include/fastpdf2png/c_api.h)
#include <fastpdf2png/c_api.h>

fpdf2png_init();
int n = fpdf2png_page_count("doc.pdf");
fpdf2png_page_c* pages = calloc(n, sizeof(fpdf2png_page_c));
fpdf2png_render("doc.pdf", 150.0f, /*no_aa=*/0, pages, n);
fpdf2png_free(pages, n);
fpdf2png_shutdown();
```

## Rules

- `third_party/libdeflate/` is patched — do not replace with upstream
- SIMD has 3 paths: NEON, SSE/AVX2, scalar — keep in sync (see `cmake/DetectSIMD.cmake`)
- Daemon protocol uses TAB separators (paths can have spaces)
- `fork()` for parallelism (PDFium is not thread-safe)
- Public headers live in `include/fastpdf2png/` — internal headers stay in `src/`
- CMake is the single build system — `scripts/build.sh` is a thin wrapper
- All new source files must be added to `CMakeLists.txt`
