// fastpdf2png - Ultra-fast PDF to PNG converter CLI
// SPDX-License-Identifier: MIT
//
// Parallelism via fork() on Unix, CreateProcess() on Windows.
// PDFium's internal caches (fonts, codecs) are not thread-safe,
// so process isolation is required for correct concurrent rendering.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <vector>
#include <chrono>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"
#include "png_writer.h"
#include "memory_pool.h"

namespace {

constexpr float kPointsPerInch = 72.0f;
constexpr int kMaxWorkers = 64;
constexpr float kMaxDpi = 2400.0f;

// ---------------------------------------------------------------------------
// Shared state — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

#ifdef _WIN32
struct SharedState {
  volatile LONG next_page;
  char pad1[60];
  volatile LONG completed_pages;
  char pad2[60];
  int total_pages;
};

inline int ClaimNextPage(SharedState* s)  { return InterlockedExchangeAdd(&s->next_page, 1); }
inline void MarkCompleted(SharedState* s) { InterlockedExchangeAdd(&s->completed_pages, 1); }
inline int GetCompleted(SharedState* s)   { return InterlockedCompareExchange(&s->completed_pages, 0, 0); }
#else
struct alignas(64) SharedState {
  std::atomic<int> next_page{0};
  char pad1[60];
  std::atomic<int> completed_pages{0};
  char pad2[60];
  int total_pages = 0;
};

inline int ClaimNextPage(SharedState* s)  { return s->next_page.fetch_add(1, std::memory_order_relaxed); }
inline void MarkCompleted(SharedState* s) { s->completed_pages.fetch_add(1, std::memory_order_relaxed); }
inline int GetCompleted(SharedState* s)   { return s->completed_pages.load(std::memory_order_relaxed); }
#endif

// ---------------------------------------------------------------------------
// PDFium helpers
// ---------------------------------------------------------------------------

void InitPdfium() {
  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
}

// ---------------------------------------------------------------------------
// Page rendering
// ---------------------------------------------------------------------------

bool RenderPage(FPDF_DOCUMENT doc, int page_idx, float dpi,
                const char* pattern, int compression) {
  auto* page = FPDF_LoadPage(doc, page_idx);
  if (!page) return false;

  const auto scale = dpi / kPointsPerInch;
  const auto width  = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
  const auto height = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);

  if (width <= 0 || height <= 0) {
    FPDF_ClosePage(page);
    return false;
  }

  const auto stride = (width * 4 + 63) & ~63;
  const auto buf_size = static_cast<size_t>(stride) * height;

  auto& pool = fast_png::GetProcessLocalPool();
  auto* buffer = pool.Acquire(buf_size);
  if (!buffer) { FPDF_ClosePage(page); return false; }

  auto* bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx, buffer, stride);
  if (!bitmap) { FPDF_ClosePage(page); return false; }

  FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0,
                        FPDF_ANNOT | FPDF_PRINTING | FPDF_NO_CATCH);

  char path[4096];
  std::snprintf(path, sizeof(path), pattern, page_idx + 1);

  const auto result = fast_png::WriteBgra(path, buffer, width, height, stride, compression);
  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);

  return result == fast_png::kSuccess;
}

// ---------------------------------------------------------------------------
// Single-process rendering
// ---------------------------------------------------------------------------

int RenderSingle(const char* pdf_path, float dpi, const char* pattern,
                 int pages, int compression) {
  auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) { std::fprintf(stderr, "Failed to open: %s\n", pdf_path); return 1; }

  int rendered = 0;
  for (int i = 0; i < pages; ++i) {
    if (RenderPage(doc, i, dpi, pattern, compression))
      ++rendered;
  }

  FPDF_CloseDocument(doc);
  return (rendered == pages) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Multi-process rendering (Unix)
// ---------------------------------------------------------------------------

#ifndef _WIN32
[[noreturn]] void WorkerLoop(const char* pdf_path, float dpi, const char* pattern,
                             int compression, SharedState* shared) {
  auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) std::exit(1);

  while (true) {
    const auto page = ClaimNextPage(shared);
    if (page >= shared->total_pages) break;
    if (RenderPage(doc, page, dpi, pattern, compression))
      MarkCompleted(shared);
  }

  FPDF_CloseDocument(doc);
  std::exit(0);
}

int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression) {
  auto* shared = static_cast<SharedState*>(
      mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (shared == MAP_FAILED) { perror("mmap"); return 1; }

  shared->next_page.store(0);
  shared->completed_pages.store(0);
  shared->total_pages = pages;

  std::vector<pid_t> children;
  children.reserve(workers);

  for (int i = 0; i < workers; ++i) {
    if (auto pid = fork(); pid == 0)
      WorkerLoop(pdf_path, dpi, pattern, compression, shared);
    else if (pid > 0)
      children.push_back(pid);
  }

  for (auto pid : children)
    waitpid(pid, nullptr, 0);

  const auto completed = GetCompleted(shared);
  munmap(shared, sizeof(SharedState));
  return (completed == pages) ? 0 : 1;
}
#endif

// ---------------------------------------------------------------------------
// Multi-process rendering (Windows)
// ---------------------------------------------------------------------------

#ifdef _WIN32
int RunWindowsWorker(const char* pdf_path, float dpi, const char* pattern,
                     int compression, const char* shm_name) {
  InitPdfium();

  auto hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
  if (!hMap) {
    std::fprintf(stderr, "Worker: OpenFileMapping failed (%lu)\n", GetLastError());
    FPDF_DestroyLibrary();
    return 1;
  }

  auto* shared = static_cast<SharedState*>(
      MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
  if (!shared) { CloseHandle(hMap); FPDF_DestroyLibrary(); return 1; }

  auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) {
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    FPDF_DestroyLibrary();
    return 1;
  }

  while (true) {
    const auto page = ClaimNextPage(shared);
    if (page >= shared->total_pages) break;
    if (RenderPage(doc, page, dpi, pattern, compression))
      MarkCompleted(shared);
  }

  FPDF_CloseDocument(doc);
  UnmapViewOfFile(shared);
  CloseHandle(hMap);
  FPDF_DestroyLibrary();
  return 0;
}

int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression) {
  char shm_name[64];
  std::snprintf(shm_name, sizeof(shm_name), "fastpdf2png_%lu",
                static_cast<unsigned long>(GetCurrentProcessId()));

  auto hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                 0, sizeof(SharedState), shm_name);
  if (!hMap) {
    std::fprintf(stderr, "CreateFileMapping failed (%lu)\n", GetLastError());
    return 1;
  }

  auto* shared = static_cast<SharedState*>(
      MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
  if (!shared) { CloseHandle(hMap); return 1; }

  shared->next_page = 0;
  shared->completed_pages = 0;
  shared->total_pages = pages;

  char exe_path[MAX_PATH];
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

  const auto dpi_x10 = static_cast<int>(dpi * 10 + 0.5f);

  std::vector<HANDLE> children;
  children.reserve(workers);

  for (int i = 0; i < workers; ++i) {
    char cmdline[8192];
    std::snprintf(cmdline, sizeof(cmdline),
                  "\"%s\" --worker \"%s\" \"%s\" %d %d \"%s\"",
                  exe_path, pdf_path, pattern, dpi_x10, compression, shm_name);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      CloseHandle(pi.hThread);
      children.push_back(pi.hProcess);
    } else {
      std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n", i, GetLastError());
    }
  }

  if (!children.empty()) {
    auto wait = WaitForMultipleObjects(
        static_cast<DWORD>(children.size()), children.data(), TRUE, INFINITE);
    if (wait == WAIT_FAILED)
      std::fprintf(stderr, "WaitForMultipleObjects failed (%lu)\n", GetLastError());
  }
  for (auto h : children) CloseHandle(h);

  const auto completed = GetCompleted(shared);
  UnmapViewOfFile(shared);
  CloseHandle(hMap);
  return (completed == pages) ? 0 : 1;
}
#endif

// ---------------------------------------------------------------------------
// Daemon mode
// ---------------------------------------------------------------------------

int SplitTabs(char* line, char** tokens, int max_tokens) {
  int count = 0;
  auto* p = line;
  while (count < max_tokens) {
    tokens[count++] = p;
    auto* tab = std::strchr(p, '\t');
    if (!tab) break;
    *tab = '\0';
    p = tab + 1;
  }
  return count;
}

int RunDaemon() {
  InitPdfium();

  char line[8192];
  while (std::fgets(line, sizeof(line), stdin)) {
    auto len = std::strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (std::strncmp(line, "QUIT", 4) == 0) break;

    char* tokens[8];
    const auto ntok = SplitTabs(line, tokens, 8);

    if (ntok >= 2 && std::string_view{tokens[0]} == "INFO") {
      auto* doc = FPDF_LoadDocument(tokens[1], nullptr);
      if (!doc) { std::printf("ERROR cannot open\n"); }
      else { std::printf("OK %d\n", FPDF_GetPageCount(doc)); FPDF_CloseDocument(doc); }
      std::fflush(stdout);
      continue;
    }

    if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
      const auto* pdf = tokens[1];
      const auto* pat = tokens[2];
      const auto dpi = (ntok >= 4) ? static_cast<float>(std::atof(tokens[3])) : 300.0f;
      const auto workers = (ntok >= 5) ? std::atoi(tokens[4]) : 1;
      const auto comp = (ntok >= 6) ? std::atoi(tokens[5]) : 2;

      auto* doc = FPDF_LoadDocument(pdf, nullptr);
      if (!doc) { std::printf("ERROR cannot open %s\n", pdf); std::fflush(stdout); continue; }
      const auto pages = FPDF_GetPageCount(doc);
      FPDF_CloseDocument(doc);

      const auto t0 = std::chrono::high_resolution_clock::now();
      const auto result = (workers > 1)
          ? RenderMulti(pdf, dpi, pat, pages, workers, comp)
          : RenderSingle(pdf, dpi, pat, pages, comp);
      const auto t1 = std::chrono::high_resolution_clock::now();
      const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

      if (result == 0) std::printf("OK %d %.3f\n", pages, elapsed);
      else std::printf("ERROR render failed\n");
      std::fflush(stdout);
      continue;
    }

    std::printf("ERROR unknown command\n");
    std::fflush(stdout);
  }

  FPDF_DestroyLibrary();
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  if (argc == 3 && std::string_view{argv[1]} == "--info") {
    InitPdfium();
    auto* doc = FPDF_LoadDocument(argv[2], nullptr);
    if (!doc) { std::fprintf(stderr, "Failed to open: %s\n", argv[2]); FPDF_DestroyLibrary(); return 1; }
    std::printf("%d\n", FPDF_GetPageCount(doc));
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return 0;
  }

  if (argc == 2 && std::string_view{argv[1]} == "--daemon")
    return RunDaemon();

#ifdef _WIN32
  if (argc == 7 && std::string_view{argv[1]} == "--worker") {
    const auto dpi = std::atoi(argv[4]) / 10.0f;
    return RunWindowsWorker(argv[2], dpi, argv[3], std::atoi(argv[5]), argv[6]);
  }
#endif

  if (argc < 3) {
    std::fprintf(stderr,
        "fastpdf2png - Ultra-fast PDF to PNG converter\n\n"
        "Usage:\n"
        "  %s input.pdf output_%%03d.png [dpi] [workers] [-c level]\n"
        "  %s --info input.pdf\n"
        "  %s --daemon\n\n"
        "Options:\n"
        "  dpi       Resolution (default: 300)\n"
        "  workers   Parallel workers (default: 1)\n"
        "  -c level  0=fast, 1=medium, 2=best (default: 2)\n",
        argv[0], argv[0], argv[0]);
    return 1;
  }

  const auto* pdf_path = argv[1];
  const auto* pattern = argv[2];
  const auto dpi = (argc > 3) ? static_cast<float>(std::atof(argv[3])) : 300.0f;
  auto workers = (argc > 4) ? std::atoi(argv[4]) : 1;
  auto compression = 2;

  for (int i = 5; i < argc; ++i) {
    if (std::string_view{argv[i]} == "-c" && i + 1 < argc) {
      compression = std::clamp(std::atoi(argv[i + 1]), 0, 2);
      break;
    }
  }

  if (dpi <= 0 || dpi > kMaxDpi) { std::fprintf(stderr, "DPI must be 1-%.0f\n", kMaxDpi); return 1; }
  workers = std::clamp(workers, 1, kMaxWorkers);

  InitPdfium();

  auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
  if (!doc) {
    std::fprintf(stderr, "Failed to open PDF: %s (error %lu)\n", pdf_path, FPDF_GetLastError());
    FPDF_DestroyLibrary();
    return 1;
  }

  const auto pages = FPDF_GetPageCount(doc);
  FPDF_CloseDocument(doc);

  if (pages <= 0) { std::fprintf(stderr, "PDF has no pages\n"); FPDF_DestroyLibrary(); return 1; }

  const auto t0 = std::chrono::high_resolution_clock::now();

  const auto result = (workers > 1)
      ? RenderMulti(pdf_path, dpi, pattern, pages, workers, compression)
      : RenderSingle(pdf_path, dpi, pattern, pages, compression);

  FPDF_DestroyLibrary();

  const auto t1 = std::chrono::high_resolution_clock::now();
  const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

  if (result == 0)
    std::printf("Rendered %d pages in %.3f seconds (%.1f pages/sec)\n",
                pages, elapsed, pages / elapsed);

  return result;
}
