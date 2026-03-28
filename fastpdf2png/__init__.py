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

from .converter import to_images, to_files, to_bytes, page_count, batch_to_files, Engine

__version__ = "2.0.0"
__all__ = ["to_images", "to_files", "to_bytes", "page_count", "batch_to_files", "Engine"]
