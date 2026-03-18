#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
SRC="$ROOT/src"
BUILD="$ROOT/build"

echo "=== Building fastpdf2png ==="

# Download PDFium if needed
if [ ! -d "$ROOT/pdfium" ]; then
    echo "Downloading PDFium..."
    "$SCRIPT_DIR/get_pdfium.sh"
fi

PDFIUM="$ROOT/pdfium"
if [ ! -d "$PDFIUM/include" ]; then
    echo "Error: PDFium not found at $PDFIUM"; exit 1
fi

CXX="${CXX:-$(command -v clang++ || command -v g++)}"
# Ensure CC matches CXX toolchain (required for LTO compatibility)
if [ -z "$CC" ] || [ "$CC" = "cc" ]; then
    case "$CXX" in
        *clang++*) CC="${CXX/clang++/clang}" ;;
        *g++*)     CC="${CXX/g++/gcc}" ;;
        *)         CC="cc" ;;
    esac
fi
OS=$(uname -s)
ARCH=$(uname -m)

CXXFLAGS="-std=c++17 -O3 -DNDEBUG -flto -Wall -Wextra -Wpedantic -Wno-unused-parameter"
CFLAGS="-O3 -DNDEBUG -flto -Wall -Wextra -Wpedantic -Wno-unused-parameter -DSUPPORT_NEAR_OPTIMAL_PARSING=0"

if [ "$ARCH" = "x86_64" ]; then
    SIMD_FLAGS="-mavx2 -msse4.1 -mssse3 -mpclmul"
    # Try x86-64-v3 first, fall back to individual flags
    if $CXX -march=x86-64-v3 -E - < /dev/null > /dev/null 2>&1; then
        SIMD_FLAGS="-march=x86-64-v3 $SIMD_FLAGS"
    fi
    CXXFLAGS="$CXXFLAGS $SIMD_FLAGS"
    CFLAGS="$CFLAGS $SIMD_FLAGS"
elif [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
    CXXFLAGS="$CXXFLAGS -mcpu=native"
    CFLAGS="$CFLAGS -mcpu=native"
fi

if [ "$OS" = "Darwin" ]; then
    LDFLAGS="-framework CoreFoundation -framework CoreGraphics -framework CoreText -lz -Wl,-rpath,@executable_path"
    LIBPDFIUM="$PDFIUM/lib/libpdfium.dylib"
else
    LDFLAGS="-lpthread -ldl -lz -Wl,-rpath,\$ORIGIN"
    LIBPDFIUM="$PDFIUM/lib/libpdfium.so"
fi
[ -f "$PDFIUM/lib/libpdfium.a" ] && LIBPDFIUM="$PDFIUM/lib/libpdfium.a"

mkdir -p "$BUILD"

# Compile libdeflate
DEFLATE="$SRC/libdeflate"
DEFLATE_SRCS="$DEFLATE/lib/deflate_compress.c $DEFLATE/lib/adler32.c $DEFLATE/lib/crc32.c $DEFLATE/lib/utils.c $DEFLATE/lib/zlib_compress.c"
[ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ] && DEFLATE_SRCS="$DEFLATE_SRCS $DEFLATE/lib/arm/cpu_features.c"
[ "$ARCH" = "x86_64" ] && DEFLATE_SRCS="$DEFLATE_SRCS $DEFLATE/lib/x86/cpu_features.c"

for f in $DEFLATE_SRCS; do
    $CC -c $CFLAGS -I"$DEFLATE" "$f" -o "$BUILD/$(basename $f .c).o" 2>/dev/null || \
    $CC -c -O3 -DNDEBUG -DSUPPORT_NEAR_OPTIMAL_PARSING=0 -I"$DEFLATE" "$f" -o "$BUILD/$(basename $f .c).o"
done

# Compile fpng
$CXX -c $CXXFLAGS "$SRC/fpng/fpng.cpp" -o "$BUILD/fpng.o" 2>/dev/null || \
$CXX -c -O3 -DNDEBUG "$SRC/fpng/fpng.cpp" -o "$BUILD/fpng.o"

# Compile source
$CXX -c $CXXFLAGS -I"$PDFIUM/include" -I"$SRC" -I"$DEFLATE" \
    "$SRC/png_writer.cpp" -o "$BUILD/png_writer.o"

$CXX -c $CXXFLAGS -I"$PDFIUM/include" -I"$SRC" \
    "$SRC/main.cpp" -o "$BUILD/main.o"

# Link
OBJS="$BUILD/main.o $BUILD/png_writer.o $BUILD/fpng.o"
for f in $DEFLATE_SRCS; do OBJS="$OBJS $BUILD/$(basename $f .c).o"; done

$CXX -O3 -flto $OBJS "$LIBPDFIUM" $LDFLAGS -o "$BUILD/fastpdf2png"

# Copy shared library if needed
if [[ "$LIBPDFIUM" == *.so ]] || [[ "$LIBPDFIUM" == *.dylib ]]; then
    cp "$LIBPDFIUM" "$BUILD/"
    [ "$OS" = "Darwin" ] && install_name_tool -change "./libpdfium.dylib" "@executable_path/libpdfium.dylib" "$BUILD/fastpdf2png"
fi

echo ""
echo "=== Build complete ==="
echo "Binary: $BUILD/fastpdf2png"
