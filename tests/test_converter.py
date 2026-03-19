"""Tests for fastpdf2png."""

import os
import struct
import tempfile
from pathlib import Path

TEST_PDF_ENV = os.environ.get("FASTPDF2PNG_TEST_PDF")
TEST_PDF = Path(TEST_PDF_ENV) if TEST_PDF_ENV else None
if TEST_PDF and not TEST_PDF.exists():
    TEST_PDF = None


def test_import():
    import fastpdf2png
    assert callable(fastpdf2png.to_images)
    assert callable(fastpdf2png.to_files)
    assert callable(fastpdf2png.to_bytes)
    assert callable(fastpdf2png.page_count)
    assert callable(fastpdf2png.Engine)


def test_to_images():
    if not TEST_PDF: return print("SKIP: no test PDF")
    import fastpdf2png
    images = fastpdf2png.to_images(TEST_PDF, dpi=72)
    assert len(images) > 0
    modes = set(img.mode for img in images)
    print(f"OK: {len(images)} images, modes={modes}")


def test_to_files():
    if not TEST_PDF: return print("SKIP: no test PDF")
    import fastpdf2png
    with tempfile.TemporaryDirectory() as d:
        files = fastpdf2png.to_files(TEST_PDF, d, dpi=72)
        assert len(files) > 0
        print(f"OK: {len(files)} files saved")


def test_to_bytes():
    if not TEST_PDF: return print("SKIP: no test PDF")
    import fastpdf2png
    data = fastpdf2png.to_bytes(TEST_PDF, dpi=72)
    assert all(b[:4] == b'\x89PNG' for b in data)
    print(f"OK: {len(data)} PNGs as bytes")


def test_page_count():
    if not TEST_PDF: return print("SKIP: no test PDF")
    import fastpdf2png
    n = fastpdf2png.page_count(TEST_PDF)
    assert n > 0
    print(f"OK: {n} pages")


def test_engine():
    if not TEST_PDF: return print("SKIP: no test PDF")
    import fastpdf2png
    with fastpdf2png.Engine() as pdf:
        n = pdf.page_count(TEST_PDF)
        assert n > 0
        images = pdf.to_images(TEST_PDF, dpi=72)
        assert len(images) == n
        print(f"OK: Engine returned {len(images)} images")


if __name__ == "__main__":
    tests = [test_import, test_to_images, test_to_files, test_to_bytes,
             test_page_count, test_engine]
    print("=== fastpdf2png tests ===\n")
    for t in tests:
        t()
        print(f"PASS: {t.__name__}\n")
    print("=== All tests passed ===")
