# AI Agent Instructions — fastpdf2png

Ultra-fast PDF to PNG converter. 49x faster than MuPDF. MIT licensed.

## Structure

```
fastpdf2png/
├── fastpdf2png/         Python package (pip install -e .)
├── src/                 All C/C++ source code
│   ├── main.cpp          CLI + daemon entry point
│   ├── png_writer.cpp   PNG encoder (SIMD, libdeflate, grayscale)
│   ├── png_writer.h
│   ├── memory_pool.h    Thread-local allocator
│   ├── libdeflate/      Bundled compression (patched, MIT)
│   └── fpng/            Bundled fast PNG encoder (Unlicense)
├── scripts/             build.sh, benchmark.sh, get_pdfium.sh
├── tests/               test_converter.py
└── .github/workflows/   CI/CD
```

`pdfium/` and `build/` are gitignored (downloaded/generated at build time).

## Build

```bash
bash scripts/build.sh
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

## Rules

- `src/libdeflate/` is patched — do not replace with upstream
- SIMD has 3 paths: NEON, SSE/AVX2, scalar — keep in sync
- Daemon protocol uses TAB separators (paths can have spaces)
- `fork()` for parallelism (PDFium is not thread-safe)
- Test: `FASTPDF2PNG_TEST_PDF=doc.pdf PYTHONPATH=. python3 tests/test_converter.py`
