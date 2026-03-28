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
#include <string>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
                        FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER |
                        FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE |
                        FPDF_RENDER_NO_SMOOTHPATH);

  char path[4096];
  std::snprintf(path, sizeof(path), pattern, page_idx + 1);

  const auto result = fast_png::WriteRgba(path, buffer, width, height, stride, compression);
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
      const auto dpi = (ntok >= 4) ? static_cast<float>(std::atof(tokens[3])) : 150.0f;
      const auto comp = (ntok >= 6) ? std::atoi(tokens[5]) : 2;

      auto* doc = FPDF_LoadDocument(pdf, nullptr);
      if (!doc) { std::printf("ERROR cannot open %s\n", pdf); std::fflush(stdout); continue; }
      const auto pages = FPDF_GetPageCount(doc);

      for (int i = 0; i < pages; ++i)
        RenderPage(doc, i, dpi, pat, comp);
      FPDF_CloseDocument(doc);

      std::printf("OK %d\n", pages);
      std::fflush(stdout);
      continue;
    }

    std::printf("ERROR unknown command\n");
    std::fflush(stdout);
  }

  FPDF_DestroyLibrary();
  return 0;
}

// ---------------------------------------------------------------------------
// Pool mode — pre-forked workers with pipe-based IPC
// Workers block on pipe read (zero CPU waste). Parent dispatches instantly.
// Large PDFs split into page ranges across workers for load balancing.
// ---------------------------------------------------------------------------

struct PipeJob {
  char pdf_path[512];
  char output_pattern[512];
  float dpi;
  int compression;
  int page_start;   // -1 = all pages, >= 0 = start page
  int page_end;     // exclusive end
};

struct PipeResult {
  int pages_rendered;
};

#ifndef _WIN32

// Read entire file into memory. Caller must free().
uint8_t* ReadFileToMemory(const char* path, size_t& out_size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return nullptr;
#ifdef __linux__
  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return nullptr; }
  auto* buf = static_cast<uint8_t*>(std::malloc(st.st_size));
  if (!buf) { close(fd); return nullptr; }
  size_t total = 0;
  while (total < static_cast<size_t>(st.st_size)) {
    auto n = read(fd, buf + total, st.st_size - total);
    if (n <= 0) break;
    total += n;
  }
  close(fd);
  out_size = total;
  return buf;
}

// Read exactly `count` bytes from fd, handling short reads.
bool ReadFull(int fd, void* buf, size_t count) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t remaining = count;
  while (remaining > 0) {
    auto n = read(fd, p, remaining);
    if (n <= 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

// Write exactly `count` bytes to fd, handling short writes.
bool WriteFull(int fd, const void* buf, size_t count) {
  auto* p = static_cast<const uint8_t*>(buf);
  size_t remaining = count;
  while (remaining > 0) {
    auto n = write(fd, p, remaining);
    if (n <= 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

[[noreturn]] void PoolWorkerLoop(int cmd_fd, int result_fd) {
  // Worker inherits PDFium from parent via fork() — font caches warm
  PipeJob job;
  while (true) {
    // Block until parent sends a full job (zero CPU waste)
    if (!ReadFull(cmd_fd, &job, sizeof(job))) break;

    // Read PDF into memory, use FPDF_LoadMemDocument64 (zero syscalls during parse)
    size_t file_size = 0;
    auto* file_data = ReadFileToMemory(job.pdf_path, file_size);

    FPDF_DOCUMENT doc = nullptr;
    if (file_data)
      doc = FPDF_LoadMemDocument64(file_data, file_size, nullptr);

    PipeResult result{0};
    if (doc) {
      const int p_start = (job.page_start >= 0) ? job.page_start : 0;
      const int p_end = (job.page_start >= 0) ? job.page_end : FPDF_GetPageCount(doc);
      for (int p = p_start; p < p_end; ++p) {
        if (RenderPage(doc, p, job.dpi, job.output_pattern, job.compression))
          ++result.pages_rendered;
      }
      FPDF_CloseDocument(doc);
    }
    std::free(file_data);

    WriteFull(result_fd, &result, sizeof(result));
  }

  close(cmd_fd);
  close(result_fd);
  FPDF_DestroyLibrary();
  std::exit(0);
}

constexpr off_t kLargeFileBytes = 200 * 1024;

int RunPool(int num_workers, float default_dpi, int default_compression) {
  InitPdfium();

  // Per-worker pipe pairs
  struct Worker {
    pid_t pid;
    int cmd_fd;     // parent writes jobs here
    int result_fd;  // parent reads results here
    int in_flight;  // jobs sent but not yet completed
  };
  std::vector<Worker> workers(num_workers);

  for (int i = 0; i < num_workers; ++i) {
    int cmd_pipe[2], result_pipe[2];
    if (pipe(cmd_pipe) != 0 || pipe(result_pipe) != 0) {
      perror("pipe");
      return 1;
    }

    auto pid = fork();
    if (pid == 0) {
      // Child: close parent ends
      close(cmd_pipe[1]);
      close(result_pipe[0]);
      // Close other workers' pipes
      for (int j = 0; j < i; ++j) {
        close(workers[j].cmd_fd);
        close(workers[j].result_fd);
      }
      PoolWorkerLoop(cmd_pipe[0], result_pipe[1]);
    }

    // Parent: close child ends
    close(cmd_pipe[0]);
    close(result_pipe[1]);
    workers[i] = {pid, cmd_pipe[1], result_pipe[0], 0};
  }

  // Build pollfd array for result pipes (non-blocking result collection)
  std::vector<struct pollfd> pollfds(num_workers);
  for (int i = 0; i < num_workers; ++i) {
    pollfds[i] = {workers[i].result_fd, POLLIN, 0};
  }

  int total_pages = 0;
  int total_jobs = 0;

  // Drain completed results (non-blocking). Must be called regularly to
  // prevent pipe buffer overflow and deadlock.
  auto drain_results = [&]() {
    while (true) {
      int ready = poll(pollfds.data(), num_workers, 0);
      if (ready <= 0) break;
      for (int i = 0; i < num_workers; ++i) {
        if (pollfds[i].revents & POLLIN) {
          PipeResult result;
          if (ReadFull(workers[i].result_fd, &result, sizeof(result))) {
            total_pages += result.pages_rendered;
            workers[i].in_flight--;
          }
        }
      }
    }
  };

  // Send job to a worker, draining results first to prevent deadlock.
  auto send_job = [&](int w, const char* pdf, const char* pat,
                       float dpi, int comp, int p_start, int p_end) {
    drain_results();
    PipeJob job{};
    std::strncpy(job.pdf_path, pdf, sizeof(job.pdf_path) - 1);
    std::strncpy(job.output_pattern, pat, sizeof(job.output_pattern) - 1);
    job.dpi = dpi;
    job.compression = comp;
    job.page_start = p_start;
    job.page_end = p_end;
    WriteFull(workers[w].cmd_fd, &job, sizeof(job));
    workers[w].in_flight++;
    total_jobs++;
  };

  // Pre-read all stdin into a job list (takes <1ms for typical input)
  struct InputJob {
    std::string pdf_path;
    std::string out_pattern;
    float dpi;
    int comp;
  };
  std::vector<InputJob> input_jobs;

  char line[8192];
  while (std::fgets(line, sizeof(line), stdin)) {
    auto len = std::strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (std::strncmp(line, "QUIT", 4) == 0) break;
    if (len == 0 || (len == 1 && line[0] == '\n')) continue;

    char* tokens[8];
    const auto ntok = SplitTabs(line, tokens, 8);

    InputJob ij;
    ij.dpi = default_dpi;
    ij.comp = default_compression;

    if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
      ij.pdf_path = tokens[1];
      ij.out_pattern = tokens[2];
      if (ntok >= 4) ij.dpi = static_cast<float>(std::atof(tokens[3]));
      if (ntok >= 6) ij.comp = std::clamp(std::atoi(tokens[5]), -1, 2);
    } else if (ntok >= 2) {
      ij.pdf_path = tokens[0];
      ij.out_pattern = tokens[1];
    } else {
      continue;
    }
    input_jobs.push_back(std::move(ij));
  }

  const auto t0 = std::chrono::high_resolution_clock::now();

  // Dispatch all jobs — round-robin across workers
  int next_worker = 0;
  for (auto& ij : input_jobs) {
    struct stat st{};
    ::stat(ij.pdf_path.c_str(), &st);

    if (st.st_size > kLargeFileBytes) {
      // Large file — split into page ranges
      auto* doc = FPDF_LoadDocument(ij.pdf_path.c_str(), nullptr);
      if (doc) {
        const int pages = FPDF_GetPageCount(doc);
        FPDF_CloseDocument(doc);
        const int ranges = std::min(pages, num_workers);
        const int per_range = (pages + ranges - 1) / ranges;
        for (int p = 0; p < pages; p += per_range) {
          send_job(next_worker % num_workers, ij.pdf_path.c_str(),
                   ij.out_pattern.c_str(), ij.dpi, ij.comp,
                   p, std::min(p + per_range, pages));
          next_worker++;
        }
      } else {
        send_job(next_worker++ % num_workers, ij.pdf_path.c_str(),
                 ij.out_pattern.c_str(), ij.dpi, ij.comp, -1, 0);
      }
    } else {
      send_job(next_worker++ % num_workers, ij.pdf_path.c_str(),
               ij.out_pattern.c_str(), ij.dpi, ij.comp, -1, 0);
    }
  }

  // Wait for all in-flight jobs to complete
  for (int i = 0; i < num_workers; ++i)
    close(workers[i].cmd_fd);  // signal shutdown (pipe close wakes blocked read)

  for (int i = 0; i < num_workers; ++i) {
    PipeResult result;
    while (workers[i].in_flight > 0) {
      if (ReadFull(workers[i].result_fd, &result, sizeof(result))) {
        total_pages += result.pages_rendered;
        workers[i].in_flight--;
      } else {
        break;  // worker died
      }
    }
    close(workers[i].result_fd);
    waitpid(workers[i].pid, nullptr, 0);
  }

  const auto t1 = std::chrono::high_resolution_clock::now();
  const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

  if (elapsed > 0)
    std::printf("Pool: %d jobs, %d pages in %.3f seconds (%.1f pages/sec)\n",
                total_jobs, total_pages, elapsed, total_pages / elapsed);

  FPDF_DestroyLibrary();
  return 0;
}

#endif

// ---------------------------------------------------------------------------
// Pool mode (Windows) — CreateProcess workers with anonymous pipe IPC
// ---------------------------------------------------------------------------

#ifdef _WIN32

uint8_t* ReadFileToMemoryWin(const char* path, size_t& out_size) {
  HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (fh == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER li;
  if (!GetFileSizeEx(fh, &li) || li.QuadPart <= 0) { CloseHandle(fh); return nullptr; }
  auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(li.QuadPart)));
  if (!buf) { CloseHandle(fh); return nullptr; }
  DWORD total = 0;
  while (total < static_cast<DWORD>(li.QuadPart)) {
    DWORD n = 0;
    if (!ReadFile(fh, buf + total, static_cast<DWORD>(li.QuadPart) - total, &n, nullptr) || n == 0) break;
    total += n;
  }
  CloseHandle(fh);
  out_size = total;
  return buf;
}

bool ReadFullWin(HANDLE h, void* buf, DWORD count) {
  auto* p = static_cast<uint8_t*>(buf);
  DWORD remaining = count;
  while (remaining > 0) {
    DWORD n = 0;
    if (!ReadFile(h, p, remaining, &n, nullptr) || n == 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

bool WriteFullWin(HANDLE h, const void* buf, DWORD count) {
  auto* p = static_cast<const uint8_t*>(buf);
  DWORD remaining = count;
  while (remaining > 0) {
    DWORD n = 0;
    if (!WriteFile(h, p, remaining, &n, nullptr) || n == 0) return false;
    p += n;
    remaining -= n;
  }
  return true;
}

int RunPoolWorkerWin(HANDLE cmd_h, HANDLE result_h) {
  InitPdfium();
  PipeJob job;
  while (true) {
    if (!ReadFullWin(cmd_h, &job, sizeof(job))) break;

    size_t file_size = 0;
    auto* file_data = ReadFileToMemoryWin(job.pdf_path, file_size);
    FPDF_DOCUMENT doc = nullptr;
    if (file_data)
      doc = FPDF_LoadMemDocument64(file_data, file_size, nullptr);

    PipeResult result{0};
    if (doc) {
      const int p_start = (job.page_start >= 0) ? job.page_start : 0;
      const int p_end = (job.page_start >= 0) ? job.page_end : FPDF_GetPageCount(doc);
      for (int p = p_start; p < p_end; ++p) {
        if (RenderPage(doc, p, job.dpi, job.output_pattern, job.compression))
          ++result.pages_rendered;
      }
      FPDF_CloseDocument(doc);
    }
    std::free(file_data);
    WriteFullWin(result_h, &result, sizeof(result));
  }
  FPDF_DestroyLibrary();
  return 0;
}

constexpr DWORD kWinLargeFileBytes = 200 * 1024;

int RunPoolWin(int num_workers, float default_dpi, int default_compression) {
  InitPdfium();

  char exe_path[MAX_PATH];
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

  struct WinWorker {
    HANDLE process;
    HANDLE cmd_write;    // parent writes jobs here
    HANDLE result_read;  // parent reads results here
    int in_flight;
  };
  std::vector<WinWorker> workers(num_workers);

  for (int i = 0; i < num_workers; ++i) {
    // Create anonymous pipes for job and result communication
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};  // inheritable
    HANDLE cmd_read, cmd_write, result_read, result_write;
    if (!CreatePipe(&cmd_read, &cmd_write, &sa, 0) ||
        !CreatePipe(&result_read, &result_write, &sa, 0)) {
      std::fprintf(stderr, "CreatePipe failed (%lu)\n", GetLastError());
      return 1;
    }
    // Don't inherit the parent-side handles
    SetHandleInformation(cmd_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(result_read, HANDLE_FLAG_INHERIT, 0);

    // Pass pipe handles to child via command line (as hex values)
    char cmdline[8192];
    std::snprintf(cmdline, sizeof(cmdline),
                  "\"%s\" --pool-worker %llu %llu",
                  exe_path,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cmd_read)),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(result_write)));

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
      std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n", i, GetLastError());
      CloseHandle(cmd_read); CloseHandle(cmd_write);
      CloseHandle(result_read); CloseHandle(result_write);
      return 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(cmd_read);      // child owns this end
    CloseHandle(result_write);  // child owns this end

    workers[i] = {pi.hProcess, cmd_write, result_read, 0};
  }

  int total_pages = 0;
  int total_jobs = 0;

  auto send_job = [&](int w, const char* pdf, const char* pat,
                       float dpi, int comp, int p_start, int p_end) {
    PipeJob job{};
    std::strncpy(job.pdf_path, pdf, sizeof(job.pdf_path) - 1);
    std::strncpy(job.output_pattern, pat, sizeof(job.output_pattern) - 1);
    job.dpi = dpi;
    job.compression = comp;
    job.page_start = p_start;
    job.page_end = p_end;
    WriteFullWin(workers[w].cmd_write, &job, sizeof(job));
    workers[w].in_flight++;
    total_jobs++;
  };

  // Pre-read all stdin
  struct InputJob {
    std::string pdf_path;
    std::string out_pattern;
    float dpi;
    int comp;
  };
  std::vector<InputJob> input_jobs;

  char line[8192];
  while (std::fgets(line, sizeof(line), stdin)) {
    auto len = std::strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (std::strncmp(line, "QUIT", 4) == 0) break;
    if (len == 0 || (len == 1 && line[0] == '\n')) continue;

    char* tokens[8];
    const auto ntok = SplitTabs(line, tokens, 8);

    InputJob ij;
    ij.dpi = default_dpi;
    ij.comp = default_compression;

    if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
      ij.pdf_path = tokens[1];
      ij.out_pattern = tokens[2];
      if (ntok >= 4) ij.dpi = static_cast<float>(std::atof(tokens[3]));
      if (ntok >= 6) ij.comp = std::clamp(std::atoi(tokens[5]), -1, 2);
    } else if (ntok >= 2) {
      ij.pdf_path = tokens[0];
      ij.out_pattern = tokens[1];
    } else {
      continue;
    }
    input_jobs.push_back(std::move(ij));
  }

  const auto t0 = std::chrono::high_resolution_clock::now();

  // Dispatch round-robin, split large PDFs
  int next_worker = 0;
  for (auto& ij : input_jobs) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExA(ij.pdf_path.c_str(), GetFileExInfoStandard, &fad);
    DWORD fsize = fad.nFileSizeLow;

    if (fsize > kWinLargeFileBytes) {
      auto* doc = FPDF_LoadDocument(ij.pdf_path.c_str(), nullptr);
      if (doc) {
        const int pages = FPDF_GetPageCount(doc);
        FPDF_CloseDocument(doc);
        const int ranges = std::min(pages, num_workers);
        const int per_range = (pages + ranges - 1) / ranges;
        for (int p = 0; p < pages; p += per_range) {
          send_job(next_worker % num_workers, ij.pdf_path.c_str(),
                   ij.out_pattern.c_str(), ij.dpi, ij.comp,
                   p, std::min(p + per_range, pages));
          next_worker++;
        }
      } else {
        send_job(next_worker++ % num_workers, ij.pdf_path.c_str(),
                 ij.out_pattern.c_str(), ij.dpi, ij.comp, -1, 0);
      }
    } else {
      send_job(next_worker++ % num_workers, ij.pdf_path.c_str(),
               ij.out_pattern.c_str(), ij.dpi, ij.comp, -1, 0);
    }
  }

  // Close cmd pipes (signals shutdown), wait for results
  for (int i = 0; i < num_workers; ++i)
    CloseHandle(workers[i].cmd_write);

  for (int i = 0; i < num_workers; ++i) {
    PipeResult result;
    while (workers[i].in_flight > 0) {
      if (ReadFullWin(workers[i].result_read, &result, sizeof(result))) {
        total_pages += result.pages_rendered;
        workers[i].in_flight--;
      } else {
        break;
      }
    }
    CloseHandle(workers[i].result_read);
    WaitForSingleObject(workers[i].process, INFINITE);
    CloseHandle(workers[i].process);
  }

  const auto t1 = std::chrono::high_resolution_clock::now();
  const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

  if (elapsed > 0)
    std::printf("Pool: %d jobs, %d pages in %.3f seconds (%.1f pages/sec)\n",
                total_jobs, total_pages, elapsed, total_pages / elapsed);

  FPDF_DestroyLibrary();
  return 0;
}
#endif

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

#ifndef _WIN32
  // --pool [dpi] [workers] [-c level]
  // Pre-forks workers, reads jobs from stdin, dispatches immediately.
  // Jobs: "pdf_path\toutput_pattern" or "RENDER\tpdf\tpattern\tdpi\tworkers\tcomp"
  if (argc >= 2 && (std::string_view{argv[1]} == "--pool" ||
                     std::string_view{argv[1]} == "--batch")) {
    const auto dpi = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 150.0f;
    auto workers = (argc > 3) ? std::atoi(argv[3]) : 4;
    auto compression = 2;
    for (int i = 4; i < argc; ++i) {
      if (std::string_view{argv[i]} == "-c" && i + 1 < argc) {
        compression = std::clamp(std::atoi(argv[i + 1]), -1, 2);
        break;
      }
    }
    workers = std::clamp(workers, 1, kMaxWorkers);
    return RunPool(workers, dpi, compression);
  }
#endif

#ifdef _WIN32
  if (argc == 4 && std::string_view{argv[1]} == "--pool-worker") {
    auto cmd_h = reinterpret_cast<HANDLE>(std::strtoull(argv[2], nullptr, 10));
    auto result_h = reinterpret_cast<HANDLE>(std::strtoull(argv[3], nullptr, 10));
    return RunPoolWorkerWin(cmd_h, result_h);
  }

  // --pool [dpi] [workers] [-c level]  (Windows version)
  if (argc >= 2 && (std::string_view{argv[1]} == "--pool" ||
                     std::string_view{argv[1]} == "--batch")) {
    const auto dpi = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 150.0f;
    auto workers = (argc > 3) ? std::atoi(argv[3]) : 4;
    auto compression = 2;
    for (int i = 4; i < argc; ++i) {
      if (std::string_view{argv[i]} == "-c" && i + 1 < argc) {
        compression = std::clamp(std::atoi(argv[i + 1]), -1, 2);
        break;
      }
    }
    workers = std::clamp(workers, 1, kMaxWorkers);
    return RunPoolWin(workers, dpi, compression);
  }

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
        "  dpi       Resolution (default: 150)\n"
        "  workers   Parallel workers (default: 4)\n"
        "  -c level  0=fast, 1=medium, 2=best (default: 2)\n",
        argv[0], argv[0], argv[0]);
    return 1;
  }

  const auto* pdf_path = argv[1];
  const auto* pattern = argv[2];
  const auto dpi = (argc > 3) ? static_cast<float>(std::atof(argv[3])) : 150.0f;
  auto workers = (argc > 4) ? std::atoi(argv[4]) : 4;
  auto compression = 2;

  for (int i = 5; i < argc; ++i) {
    if (std::string_view{argv[i]} == "-c" && i + 1 < argc) {
      compression = std::clamp(std::atoi(argv[i + 1]), -1, 2);
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
