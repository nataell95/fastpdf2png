"""
fastpdf2png — Ultra-fast PDF to PNG converter.

    import fastpdf2png

    images = fastpdf2png.to_images("doc.pdf")           # PIL images
    files  = fastpdf2png.to_files("doc.pdf", "output/")  # save to folder
    data   = fastpdf2png.to_bytes("doc.pdf")             # raw PNG bytes
    n      = fastpdf2png.page_count("doc.pdf")           # number of pages

    # Batch processing (keeps engine warm):
    with fastpdf2png.Engine() as pdf:
        for path in my_files:
            images = pdf.to_images(path)
"""

from .converter import to_images, to_files, to_bytes, page_count, Engine

__version__ = "1.2.0"
__all__ = ["to_images", "to_files", "to_bytes", "page_count", "Engine"]
