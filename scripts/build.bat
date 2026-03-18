@echo off
setlocal enabledelayedexpansion

set SCRIPT_DIR=%~dp0
set ROOT=%SCRIPT_DIR%..
set SRC=%ROOT%\src
set BUILD=%ROOT%\build

echo === Building fastpdf2png (Windows) ===

:: Download PDFium if needed
if not exist "%ROOT%\pdfium" (
    echo Downloading PDFium...
    call "%SCRIPT_DIR%get_pdfium.bat"
)

set PDFIUM=%ROOT%\pdfium
if not exist "%PDFIUM%\include" (
    echo Error: PDFium not found at %PDFIUM%
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

set CXXFLAGS=/O2 /DNDEBUG /EHsc /std:c++17 /MD /GL /W4 /wd4100
set CFLAGS=/O2 /DNDEBUG /DSUPPORT_NEAR_OPTIMAL_PARSING=0 /GL /W4 /wd4100

:: Detect AVX2 support at build time
set SIMD_FLAGS=
where wmic >nul 2>&1 && for /f "tokens=*" %%a in ('wmic cpu get Caption /value 2^>nul ^| find "Intel"') do set SIMD_FLAGS=/arch:AVX2
:: Also check AMD
where wmic >nul 2>&1 && for /f "tokens=*" %%a in ('wmic cpu get Caption /value 2^>nul ^| find "AMD"') do set SIMD_FLAGS=/arch:AVX2
if defined SIMD_FLAGS (
    echo Detected x86_64 CPU with AVX2, enabling SIMD
    set CXXFLAGS=%CXXFLAGS% %SIMD_FLAGS%
    set CFLAGS=%CFLAGS% %SIMD_FLAGS%
) else (
    echo No AVX2 detected, building without SIMD
)

:: Compile libdeflate
set DEFLATE=%SRC%\libdeflate
set DEFLATE_SRCS=deflate_compress.c adler32.c crc32.c utils.c zlib_compress.c
for %%f in (%DEFLATE_SRCS%) do (
    cl /c %CFLAGS% /I"%DEFLATE%" "%DEFLATE%\lib\%%f" /Fo"%BUILD%\%%~nf.obj" >nul 2>&1
)
cl /c %CFLAGS% /I"%DEFLATE%" "%DEFLATE%\lib\x86\cpu_features.c" /Fo"%BUILD%\cpu_features.obj" >nul 2>&1

:: Compile fpng
cl /c %CXXFLAGS% "%SRC%\fpng\fpng.cpp" /Fo"%BUILD%\fpng.obj" >nul 2>&1

:: Compile source
cl /c %CXXFLAGS% /I"%PDFIUM%\include" /I"%SRC%" /I"%DEFLATE%" "%SRC%\png_writer.cpp" /Fo"%BUILD%\png_writer.obj"
cl /c %CXXFLAGS% /I"%PDFIUM%\include" /I"%SRC%" "%SRC%\main.cpp" /Fo"%BUILD%\main.obj"

:: Link
set OBJS=%BUILD%\main.obj %BUILD%\png_writer.obj %BUILD%\fpng.obj
for %%f in (%DEFLATE_SRCS%) do set OBJS=!OBJS! %BUILD%\%%~nf.obj
set OBJS=%OBJS% %BUILD%\cpu_features.obj

set LIBPDFIUM=%PDFIUM%\lib\pdfium.dll.lib
if exist "%PDFIUM%\lib\pdfium.lib" set LIBPDFIUM=%PDFIUM%\lib\pdfium.lib

link /OUT:"%BUILD%\fastpdf2png.exe" %OBJS% "%LIBPDFIUM%" /NOLOGO /LTCG

:: Copy DLL
if exist "%PDFIUM%\bin\pdfium.dll" copy "%PDFIUM%\bin\pdfium.dll" "%BUILD%\" >nul

echo.
echo === Build complete ===
echo Binary: %BUILD%\fastpdf2png.exe
