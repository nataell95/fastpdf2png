"""Core converter — wraps the fastpdf2png binary."""

import atexit
import io
import os
import subprocess
import tempfile
from pathlib import Path
from typing import List, Union

_PKG_DIR = Path(__file__).parent
_ROOT_DIR = _PKG_DIR.parent
_AUTO_WORKERS = min(4, max(1, os.cpu_count() or 1))


def _find_binary() -> Path:
    import sys
    ext = ".exe" if sys.platform == "win32" else ""
    for p in [
        _PKG_DIR / "bin" / f"fastpdf2png{ext}",     # installed via pip
        _ROOT_DIR / "build" / f"fastpdf2png{ext}",   # built from source
        Path(f"/usr/local/bin/fastpdf2png{ext}"),
    ]:
        if p.exists():
            return p
    raise FileNotFoundError(
        "fastpdf2png binary not found. Run: bash scripts/build.sh"
    )


# ---------------------------------------------------------------------------
# Simple API
# ---------------------------------------------------------------------------

def to_images(pdf: Union[str, Path], dpi: int = 150, workers: int = None) -> list:
    """
    Convert a PDF to a list of PIL images.

    Args:
        pdf: Path to the PDF file.
        dpi: Resolution (default 150). Use 300 for print quality.
        workers: Parallel worker processes (default: auto, max 4).

    Returns:
        List of PIL.Image.Image — one per page.
        Text pages are grayscale (mode='L'), color pages are RGB.

    Example:
        images = fastpdf2png.to_images("report.pdf")
        images[0].show()
        images[0].save("cover.png")
    """
    try:
        from PIL import Image
    except ImportError:
        raise ImportError(
            "Pillow is required: pip install Pillow\n"
            "Or use fastpdf2png.to_bytes() for raw PNG data."
        )
    return [Image.open(io.BytesIO(b)) for b in to_bytes(pdf, dpi=dpi, workers=workers)]


def to_files(
    pdf: Union[str, Path],
    output_dir: Union[str, Path],
    dpi: int = 150,
    prefix: str = "page_",
    workers: int = None,
) -> List[Path]:
    """
    Convert a PDF to PNG files on disk.

    Args:
        pdf: Path to the PDF file.
        output_dir: Folder to save PNGs into.
        dpi: Resolution (default 150).
        prefix: Filename prefix (default "page_").
        workers: Parallel worker processes (default: auto, max 4).

    Returns:
        Sorted list of PNG file paths.

    Example:
        fastpdf2png.to_files("report.pdf", "output/")
        # Creates: output/page_001.png, output/page_002.png, ...
    """
    return _run_render(pdf, output_dir, dpi, prefix, workers)


def to_bytes(pdf: Union[str, Path], dpi: int = 150, workers: int = None) -> List[bytes]:
    """
    Convert a PDF to PNG bytes in memory.

    Args:
        pdf: Path to the PDF file.
        dpi: Resolution (default 150).
        workers: Parallel worker processes (default: auto, max 4).

    Returns:
        List of PNG data as bytes — one per page.

    Example:
        data = fastpdf2png.to_bytes("report.pdf")
        with open("page1.png", "wb") as f:
            f.write(data[0])
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        files = _run_render(pdf, tmpdir, dpi, workers=workers)
        return [f.read_bytes() for f in files]


def to_raw(
    pdf: Union[str, Path], dpi: int = 150, workers: int = 1,
) -> list:
    """
    Convert a PDF to raw BGR pixel buffers (no PNG encoding).

    Args:
        pdf: Path to the PDF file.
        dpi: Resolution (default 150).
        workers: Unused (kept for API symmetry). Raw mode is single-process.

    Returns:
        List of (bgr_bytes, width, height) tuples — one per page.
        bgr_bytes is width*height*3 bytes in BGR order, row-major.

    Example:
        pages = fastpdf2png.to_raw("report.pdf", dpi=100)
        bgr, w, h = pages[0]
    """
    binary = _find_binary()
    result = subprocess.run(
        [str(binary), "--raw-stdout", str(Path(pdf).resolve()), str(dpi)],
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Raw conversion failed: {result.stderr.decode()}")

    pages = []
    buf = result.stdout
    pos = 0
    while pos < len(buf):
        if pos + 8 > len(buf):
            break
        w = int.from_bytes(buf[pos:pos + 4], "little"); pos += 4
        h = int.from_bytes(buf[pos:pos + 4], "little"); pos += 4
        size = w * h * 3
        if pos + size > len(buf):
            raise RuntimeError(
                f"Truncated raw output: expected {size} bytes for {w}x{h} page, "
                f"got {len(buf) - pos}"
            )
        pixels = buf[pos:pos + size]; pos += size
        pages.append((pixels, w, h))
    return pages


def page_count(pdf: Union[str, Path]) -> int:
    """
    Get the number of pages in a PDF (instant, no rendering).

    Example:
        n = fastpdf2png.page_count("report.pdf")  # → 71
    """
    binary = _find_binary()
    result = subprocess.run(
        [str(binary), "--info", str(Path(pdf).resolve())],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Failed to read PDF: {result.stderr}")
    return int(result.stdout.strip())


def _run_render(pdf, output_dir, dpi=150, prefix="page_", workers=None):
    binary = _find_binary()
    pdf = Path(pdf).resolve()
    output_dir = Path(output_dir).resolve()
    w = workers if workers is not None else _AUTO_WORKERS

    if not pdf.exists():
        raise FileNotFoundError(f"PDF not found: {pdf}")

    output_dir.mkdir(parents=True, exist_ok=True)
    pattern = str(output_dir / f"{prefix}%03d.png")

    result = subprocess.run(
        [str(binary), str(pdf), pattern, str(dpi), str(w), "-c", "2"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Conversion failed: {result.stderr}")

    return sorted(output_dir.glob(f"{prefix}*.png"))


# ---------------------------------------------------------------------------
# Engine (persistent process for batch)
# ---------------------------------------------------------------------------

class Engine:
    """
    Persistent PDF engine for batch processing.

    Keeps PDFium loaded between calls — no startup cost after the first.

    Usage:
        with fastpdf2png.Engine() as pdf:
            for path in my_pdf_files:
                images = pdf.to_images(path)
    """

    def __init__(self):
        binary = _find_binary()
        self._proc = subprocess.Popen(
            [str(binary), "--daemon"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        atexit.register(self.close)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def close(self):
        """Shut down the engine."""
        atexit.unregister(self.close)
        if self._proc and self._proc.poll() is None:
            try:
                self._proc.stdin.write("QUIT\n")
                self._proc.stdin.flush()
                self._proc.wait(timeout=5)
            except (BrokenPipeError, OSError):
                self._proc.kill()
        self._proc = None

    def _cmd(self, command: str) -> str:
        if not self._proc or self._proc.poll() is not None:
            raise RuntimeError("Engine is closed")
        self._proc.stdin.write(command + "\n")
        self._proc.stdin.flush()
        line = self._proc.stdout.readline().strip()
        if line.startswith("ERROR"):
            raise RuntimeError(line)
        return line

    def to_images(self, pdf: Union[str, Path], dpi: int = 150, workers: int = None) -> list:
        """Convert a PDF to PIL images."""
        try:
            from PIL import Image
        except ImportError:
            raise ImportError("Pillow required: pip install Pillow")
        return [Image.open(io.BytesIO(b)) for b in self.to_bytes(pdf, dpi=dpi, workers=workers)]

    def to_files(self, pdf: Union[str, Path], output_dir: Union[str, Path],
                 dpi: int = 150, prefix: str = "page_", workers: int = None) -> List[Path]:
        """Convert a PDF to PNG files on disk."""
        pdf = str(Path(pdf).resolve())
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        pattern = str(output_dir / f"{prefix}%03d.png")
        w = workers if workers is not None else _AUTO_WORKERS
        self._cmd(f"RENDER\t{pdf}\t{pattern}\t{dpi}\t{w}\t2")
        return sorted(output_dir.glob(f"{prefix}*.png"))

    def to_bytes(self, pdf: Union[str, Path], dpi: int = 150, workers: int = None) -> List[bytes]:
        """Convert a PDF to PNG bytes in memory."""
        with tempfile.TemporaryDirectory() as tmpdir:
            files = self.to_files(pdf, tmpdir, dpi=dpi, workers=workers)
            return [f.read_bytes() for f in files]

    def to_raw(self, pdf: Union[str, Path], dpi: int = 150, workers: int = 1) -> list:
        """Convert a PDF to raw BGR pixel buffers (no PNG encoding).

        Returns list of (bgr_bytes, width, height) tuples.
        Note: Uses a separate subprocess (not the daemon pipe) because
        raw binary output on stdout conflicts with the daemon text protocol.
        """
        return to_raw(pdf, dpi=dpi, workers=workers)

    def page_count(self, pdf: Union[str, Path]) -> int:
        """Get page count (instant)."""
        resp = self._cmd(f"INFO\t{Path(pdf).resolve()}")
        return int(resp.split()[1])
