"""Native C library binding — zero-copy raw RGBA rendering via ctypes."""

import ctypes
import os
import sys
from pathlib import Path
from typing import List, Optional

# Find the shared library
_LIB = None


class _PageC(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("stride", ctypes.c_int32),
    ]


def _load_lib():
    global _LIB
    if _LIB is not None:
        return _LIB

    pkg_dir = Path(__file__).parent
    root_dir = pkg_dir.parent

    if sys.platform == "darwin":
        lib_name = "libfastpdf2png.dylib"
    elif sys.platform == "win32":
        lib_name = "fastpdf2png.dll"
    else:
        lib_name = "libfastpdf2png.so"

    for d in [pkg_dir / "bin", root_dir / "build", Path("/usr/local/lib")]:
        p = d / lib_name
        if p.exists():
            _LIB = ctypes.CDLL(str(p))
            _LIB.fpdf2png_init()

            # Set argtypes/restype once at load time (not per-call)
            _LIB.fpdf2png_render.argtypes = [
                ctypes.c_char_p, ctypes.c_float, ctypes.c_int,
                ctypes.POINTER(ctypes.POINTER(_PageC)), ctypes.POINTER(ctypes.c_int)
            ]
            _LIB.fpdf2png_render.restype = ctypes.c_int
            _LIB.fpdf2png_free.argtypes = [ctypes.POINTER(_PageC), ctypes.c_int]
            _LIB.fpdf2png_page_count.argtypes = [ctypes.c_char_p]
            _LIB.fpdf2png_page_count.restype = ctypes.c_int

            return _LIB

    raise FileNotFoundError(f"{lib_name} not found. Run: bash scripts/build.sh")


class PageBuffer:
    """Raw RGBA pixel buffer from a rendered PDF page."""
    __slots__ = ("_data", "width", "height", "stride", "channels")

    def __init__(self, data: bytes, width: int, height: int, stride: int):
        self._data = data
        self.width = width
        self.height = height
        self.stride = stride
        self.channels = 4

    @property
    def data(self) -> bytes:
        return self._data

    def to_numpy(self):
        """Convert to numpy array (height, width, 4) RGBA."""
        import numpy as np
        arr = np.frombuffer(self._data, dtype=np.uint8).reshape(self.height, self.stride // 4, 4)
        return arr[:, :self.width, :].copy()

    def to_pil(self):
        """Convert to PIL Image."""
        from PIL import Image
        arr = self.to_numpy()
        return Image.fromarray(arr[:, :, :3], "RGB")  # drop alpha


def _render_one(args):
    """Worker function for ProcessPoolExecutor — runs in a separate process."""
    pdf_path, dpi, no_aa = args
    lib = _load_lib()

    pages_ptr = ctypes.POINTER(_PageC)()
    count = ctypes.c_int(0)

    path = str(Path(pdf_path).resolve()).encode()
    err = lib.fpdf2png_render(path, dpi, 1 if no_aa else 0,
                               ctypes.byref(pages_ptr), ctypes.byref(count))
    if err != 0:
        return []

    result = []
    for i in range(count.value):
        p = pages_ptr[i]
        buf_size = p.stride * p.height
        data = ctypes.string_at(p.data, buf_size)
        result.append(PageBuffer(data, p.width, p.height, p.stride))

    lib.fpdf2png_free(pages_ptr, count)
    return result


def render(pdf_path: str, dpi: float = 150, no_aa: bool = False) -> List[PageBuffer]:
    """
    Render PDF to raw RGBA pixel buffers. No PNG encoding, no disk I/O.

    Args:
        pdf_path: Path to PDF file.
        dpi: Resolution (default 150).
        no_aa: Disable anti-aliasing for ~70% more speed.

    Returns:
        List of PageBuffer objects with raw RGBA pixels.
    """
    return _render_one((pdf_path, dpi, no_aa))


def render_many(pdf_paths: List[str], dpi: float = 150, no_aa: bool = False,
                workers: int = None) -> List[List[PageBuffer]]:
    """
    Render many PDFs in parallel using process pool. Maximum throughput.
    No PNG encoding, no disk I/O, no subprocess overhead.

    Args:
        pdf_paths: List of PDF file paths.
        dpi: Resolution (default 150).
        no_aa: Disable anti-aliasing for speed.
        workers: Number of parallel processes (default: CPU count).

    Returns:
        List of lists — one list of PageBuffer per input PDF.
    """
    from concurrent.futures import ProcessPoolExecutor

    w = workers or min(8, os.cpu_count() or 4)
    args = [(p, dpi, no_aa) for p in pdf_paths]

    with ProcessPoolExecutor(max_workers=w) as pool:
        return list(pool.map(_render_one, args))


def page_count(pdf_path: str) -> int:
    """Get page count without rendering."""
    lib = _load_lib()
    return lib.fpdf2png_page_count(str(Path(pdf_path).resolve()).encode())
