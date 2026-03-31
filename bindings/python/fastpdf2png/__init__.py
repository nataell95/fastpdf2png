"""
fastpdf2png — Ultra-fast PDF to PNG converter.

    import fastpdf2png

    images = fastpdf2png.to_images("doc.pdf")           # PIL images
    files  = fastpdf2png.to_files("doc.pdf", "output/")  # save to folder
    data   = fastpdf2png.to_bytes("doc.pdf")             # raw PNG bytes
    n      = fastpdf2png.page_count("doc.pdf")           # number of pages

    # High-throughput streaming:
    with fastpdf2png.Engine(workers=8) as eng:
        eng.to_files_many(pdf_list, "output/")
"""

from .converter import to_images, to_files, to_bytes, to_raw, page_count, batch_to_files, Engine

from pathlib import Path as _Path

def _read_version() -> str:
    """Read version from VERSION file, with fallback."""
    version_file = _Path(__file__).parent.parent.parent.parent / "VERSION"
    try:
        return version_file.read_text().strip()
    except FileNotFoundError:
        return "2.0.0"

__version__ = _read_version()
__all__ = ["to_images", "to_files", "to_bytes", "to_raw", "page_count", "batch_to_files", "Engine"]
