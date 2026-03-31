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
# Project root when running from source: bindings/python/fastpdf2png -> ../../..
_PROJECT_ROOT = _PKG_DIR.parent.parent.parent
_AUTO_WORKERS = min(4, max(1, os.cpu_count() or 1))


def _find_binary() -> Path:
    import shutil
    import sys
    ext = ".exe" if sys.platform == "win32" else ""
    name = f"fastpdf2png{ext}"
    for p in [
        _PKG_DIR / "bin" / name,              # installed via pip
        _PROJECT_ROOT / "build" / name,        # built from source (cmake --preset release)
        _ROOT_DIR / "build" / name,            # legacy location
    ]:
        if p.exists():
            return p
    # Check PATH
    found = shutil.which("fastpdf2png")
    if found:
        return Path(found)
    raise FileNotFoundError(
        "fastpdf2png binary not found. Run: cmake --preset release && cmake --build build"
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


def to_raw(pdf: Union[str, Path], dpi: int = 150, workers: int = None) -> list:
    """
    Convert a PDF to raw RGBA pixel buffers in memory.
    No PNG encoding, no disk I/O — maximum speed.

    Args:
        pdf: Path to the PDF file.
        dpi: Resolution (default 150).
        workers: Parallel worker processes (default: auto, max 4).

    Returns:
        List of dicts with keys: 'data' (bytes), 'width', 'height', 'stride', 'channels'.
        Pixel format is RGBA (4 bytes per pixel).

    Example:
        pages = fastpdf2png.to_raw("report.pdf", dpi=150)
        # Feed to GPU, numpy, OpenCV, etc.
        import numpy as np
        arr = np.frombuffer(pages[0]['data'], dtype=np.uint8).reshape(
            pages[0]['height'], pages[0]['stride'] // pages[0]['channels'], pages[0]['channels'])
    """
    import struct

    binary = _find_binary()
    pdf = Path(pdf).resolve()
    if not pdf.exists():
        raise FileNotFoundError(f"PDF not found: {pdf}")

    def _read_raw_stdout(proc):
        """Read PixelHeader + RGBA pixels from a --raw process."""
        result = []
        hdr_size = 16
        while True:
            hdr_bytes = proc.stdout.read(hdr_size)
            if len(hdr_bytes) < hdr_size:
                break
            width, height, stride, channels = struct.unpack('iiii', hdr_bytes)
            data = proc.stdout.read(stride * height)
            if len(data) < stride * height:
                break
            result.append({
                'data': data, 'width': width, 'height': height,
                'stride': stride, 'channels': channels,
            })
        proc.wait()
        return result

    # --raw streams PixelHeader + RGBA pixels to stdout. No disk I/O.
    proc = subprocess.Popen(
        [str(binary), "--raw", str(pdf), str(dpi)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return _read_raw_stdout(proc)


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


def batch_to_files(
    pdfs: List[Union[str, Path]],
    output_dir: Union[str, Path],
    dpi: int = 150,
    workers: int = None,
) -> List[Path]:
    """
    Convert many PDFs to PNGs in a single invocation — much faster for
    batches of small files since PDFium is initialized only once and
    fork workers process all PDFs in parallel.

    Args:
        pdfs: List of PDF file paths.
        output_dir: Folder to save PNGs into.
        dpi: Resolution (default 150).
        workers: Parallel worker processes (default: auto, max 4).

    Returns:
        Sorted list of all generated PNG file paths.
    """
    binary = _find_binary()
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    w = workers if workers is not None else _AUTO_WORKERS

    # Build stdin: each line is "pdf_path\toutput_pattern"
    lines = []
    for pdf in pdfs:
        pdf = Path(pdf).resolve()
        if not pdf.exists():
            continue
        stem = pdf.stem
        pattern = str(output_dir / f"{stem}_%03d.png")
        lines.append(f"{pdf}\t{pattern}")

    if not lines:
        return []

    stdin_data = "\n".join(lines) + "\n"

    result = subprocess.run(
        [str(binary), "--batch", str(dpi), str(w), "-c", "2"],
        input=stdin_data,
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"Batch conversion failed: {result.stderr}")

    return sorted(output_dir.glob("*.png"))


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
# Engine — pool of hot daemon processes
# ---------------------------------------------------------------------------

class _DaemonProc:
    """Single persistent daemon subprocess with PDFium loaded."""

    def __init__(self, binary: Path):
        self._proc = subprocess.Popen(
            [str(binary), "--daemon"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def cmd(self, command: str) -> str:
        if not self._proc or self._proc.poll() is not None:
            raise RuntimeError("Daemon process died")
        self._proc.stdin.write(command + "\n")
        self._proc.stdin.flush()
        line = self._proc.stdout.readline().strip()
        if line.startswith("ERROR"):
            raise RuntimeError(line)
        return line

    def pipeline(self, commands: List[str]) -> List[str]:
        """Send all commands at once, then read all responses. No round-trip per command."""
        if not self._proc or self._proc.poll() is not None:
            raise RuntimeError("Daemon process died")
        # Write all commands in one flush
        self._proc.stdin.write("\n".join(commands) + "\n")
        self._proc.stdin.flush()
        # Read all responses
        results = []
        for _ in commands:
            line = self._proc.stdout.readline().strip()
            results.append(line)
        return results

    def close(self):
        if self._proc and self._proc.poll() is None:
            try:
                self._proc.stdin.write("QUIT\n")
                self._proc.stdin.flush()
                self._proc.wait(timeout=5)
            except (BrokenPipeError, OSError):
                self._proc.kill()
        self._proc = None


class Engine:
    """
    Pool of hot PDFium processes for high-throughput batch processing.

    Spawns N daemon processes at startup, each with PDFium already loaded.
    PDFs are distributed across daemons in parallel — no startup cost per file.

    Usage:
        with fastpdf2png.Engine(workers=8) as pdf:
            # Single file
            images = pdf.to_images("doc.pdf")

            # Many files — processed in parallel across all workers
            pdf.to_files_many(pdf_list, "output/")
    """

    def __init__(self, workers: int = None):
        binary = _find_binary()
        w = workers if workers is not None else _AUTO_WORKERS
        self._daemons = [_DaemonProc(binary) for _ in range(w)]
        self._next = 0
        atexit.register(self.close)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def close(self):
        """Shut down all daemon processes."""
        atexit.unregister(self.close)
        for d in self._daemons:
            d.close()
        self._daemons = []

    def _pick_daemon(self) -> _DaemonProc:
        d = self._daemons[self._next % len(self._daemons)]
        self._next += 1
        return d

    def to_images(self, pdf: Union[str, Path], dpi: int = 150) -> list:
        """Convert a PDF to PIL images."""
        try:
            from PIL import Image
        except ImportError:
            raise ImportError("Pillow required: pip install Pillow")
        return [Image.open(io.BytesIO(b)) for b in self.to_bytes(pdf, dpi=dpi)]

    def to_files(self, pdf: Union[str, Path], output_dir: Union[str, Path],
                 dpi: int = 150, prefix: str = "page_") -> List[Path]:
        """Convert a single PDF to PNG files on disk."""
        pdf = str(Path(pdf).resolve())
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        pattern = str(output_dir / f"{prefix}%03d.png")
        self._pick_daemon().cmd(f"RENDER\t{pdf}\t{pattern}\t{dpi}\t1\t2")
        return sorted(output_dir.glob(f"{prefix}*.png"))

    def to_files_many(self, pdfs: List[Union[str, Path]],
                      output_dir: Union[str, Path],
                      dpi: int = 150) -> List[Path]:
        """Convert many PDFs in parallel using the existing daemon pool.

        Distributes RENDER commands across all daemon processes using
        pipeline() for maximum throughput — no subprocess spawning overhead.
        """
        output_dir = Path(output_dir).resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        # Build RENDER commands for each PDF
        commands_by_daemon: dict = {i: [] for i in range(len(self._daemons))}
        idx = 0
        for pdf in pdfs:
            pdf = Path(pdf).resolve()
            if not pdf.exists():
                continue
            pattern = str(output_dir / f"{pdf.stem}_%03d.png")
            cmd = f"RENDER\t{pdf}\t{pattern}\t{dpi}\t1\t2"
            daemon_idx = idx % len(self._daemons)
            commands_by_daemon[daemon_idx].append(cmd)
            idx += 1

        # Send commands to each daemon via pipeline (batch write + batch read)
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(self._daemons)) as pool:
            futures = []
            for i, cmds in commands_by_daemon.items():
                if cmds:
                    futures.append(pool.submit(self._daemons[i].pipeline, cmds))
            # Wait for all daemons to finish and check for errors
            for f in concurrent.futures.as_completed(futures):
                results = f.result()
                for line in results:
                    if line.startswith("ERROR"):
                        raise RuntimeError(f"Daemon error: {line}")

        return sorted(output_dir.glob("*.png"))

    def to_bytes(self, pdf: Union[str, Path], dpi: int = 150) -> List[bytes]:
        """Convert a PDF to PNG bytes in memory."""
        with tempfile.TemporaryDirectory() as tmpdir:
            files = self.to_files(pdf, tmpdir, dpi=dpi)
            return [f.read_bytes() for f in files]

    def page_count(self, pdf: Union[str, Path]) -> int:
        """Get page count (instant)."""
        resp = self._pick_daemon().cmd(f"INFO\t{Path(pdf).resolve()}")
        return int(resp.split()[1])
