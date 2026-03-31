// fastpdf2png — Pool mode (pre-forked workers with pipe IPC)
// SPDX-License-Identifier: MIT

#include "cli/pool_cmd.h"
#include "cli/protocol.h"
#include "internal/pdfium_render.h"
#include "internal/file_io.h"
#include "png/memory_pool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png::cli {

namespace {

constexpr float kPointsPerInch = 72.0f;
constexpr int kNoAA = FPDF_RENDER_NO_SMOOTHTEXT |
                      FPDF_RENDER_NO_SMOOTHIMAGE |
                      FPDF_RENDER_NO_SMOOTHPATH;

void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

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

} // anonymous namespace

// ===========================================================================
// Unix pool mode
// ===========================================================================

#ifndef _WIN32

namespace {

constexpr off_t kLargeFileBytes = 500 * 1024;

[[noreturn]] void PoolWorkerLoop(int cmd_fd, int result_fd) {
    // Worker inherits PDFium from parent via fork() — font caches warm

    // Cache last-loaded PDF to avoid re-reading for split page ranges
    char cached_path[512] = {};
    uint8_t* cached_data = nullptr;
    size_t cached_size = 0;
    FPDF_DOCUMENT cached_doc = nullptr;

    PipeJob job;
    while (true) {
        if (!internal::ReadFull(cmd_fd, &job, sizeof(job))) break;

        // Reuse cached doc if same PDF (common for split page ranges)
        FPDF_DOCUMENT doc;
        if (cached_doc && std::strcmp(job.pdf_path, cached_path) == 0) {
            doc = cached_doc;
        } else {
            if (cached_doc) FPDF_CloseDocument(cached_doc);
            std::free(cached_data);

            cached_data = internal::ReadFileToMemory(job.pdf_path, cached_size);
            doc = cached_data
                ? FPDF_LoadMemDocument64(cached_data, cached_size, nullptr)
                : nullptr;
            cached_doc = doc;
            std::strncpy(cached_path, job.pdf_path, sizeof(cached_path) - 1);
        }

        const bool memory_mode = (job.output_pattern[0] == '\0');
        PipeResult result{0};

        if (doc) {
            const int p_start = (job.page_start >= 0) ? job.page_start : 0;
            const int p_end = (job.page_start >= 0)
                ? job.page_end : FPDF_GetPageCount(doc);

            for (int p = p_start; p < p_end; ++p) {
                if (memory_mode) {
                    // Memory mode: render + send raw pixels through pipe
                    auto* page = FPDF_LoadPage(doc, p);
                    if (!page) continue;

                    const auto scale = job.dpi / kPointsPerInch;
                    const auto w = static_cast<int>(
                        FPDF_GetPageWidth(page) * scale + 0.5f);
                    const auto h = static_cast<int>(
                        FPDF_GetPageHeight(page) * scale + 0.5f);
                    if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

                    const auto stride = (w * 4 + 63) & ~63;
                    const auto buf_size = static_cast<size_t>(stride) * h;

                    auto& pool = fast_png::GetProcessLocalPool();
                    auto* buffer = pool.Acquire(buf_size);
                    if (!buffer) { FPDF_ClosePage(page); continue; }

                    auto* bitmap = FPDFBitmap_CreateEx(
                        w, h, FPDFBitmap_BGRx, buffer, stride);
                    if (!bitmap) { FPDF_ClosePage(page); continue; }

                    int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
                    if (job.no_aa) flags |= kNoAA;
                    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
                    FPDFBitmap_Destroy(bitmap);
                    FPDF_ClosePage(page);

                    // Send header + raw RGBA pixels
                    PixelHeader hdr{w, h, stride, 4};
                    internal::WriteFull(result_fd, &hdr, sizeof(hdr));
                    internal::WriteFull(result_fd, buffer, buf_size);
                    ++result.pages_rendered;
                } else {
                    // File mode: render + encode + write to disk
                    if (internal::RenderPageToFile(doc, p, job.dpi,
                            job.output_pattern, job.compression, job.no_aa))
                        ++result.pages_rendered;
                }
            }
        }

        if (!memory_mode)
            internal::WriteFull(result_fd, &result, sizeof(result));
    }

    if (cached_doc) FPDF_CloseDocument(cached_doc);
    std::free(cached_data);
    close(cmd_fd);
    close(result_fd);
    FPDF_DestroyLibrary();
    std::exit(0);
}

} // anonymous namespace

int RunPool(int num_workers, float default_dpi, int default_compression, bool no_aa) {
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

    // Drain completed results (non-blocking)
    auto drain_results = [&]() {
        while (true) {
            int ready = poll(pollfds.data(), num_workers, 0);
            if (ready <= 0) break;
            for (int i = 0; i < num_workers; ++i) {
                if (pollfds[i].revents & POLLIN) {
                    PipeResult result;
                    if (internal::ReadFull(workers[i].result_fd,
                                           &result, sizeof(result))) {
                        total_pages += result.pages_rendered;
                        workers[i].in_flight--;
                    }
                }
            }
        }
    };

    // Pick the worker with fewest in-flight jobs
    auto pick_worker = [&]() -> int {
        int best = 0;
        for (int i = 1; i < num_workers; ++i)
            if (workers[i].in_flight < workers[best].in_flight) best = i;
        return best;
    };

    // Send job to a worker
    auto send_job = [&](int w, const char* pdf, const char* pat,
                         float dpi, int comp, int p_start, int p_end) {
        PipeJob job{};
        std::strncpy(job.pdf_path, pdf, sizeof(job.pdf_path) - 1);
        std::strncpy(job.output_pattern, pat, sizeof(job.output_pattern) - 1);
        job.dpi = dpi;
        job.compression = comp;
        job.page_start = p_start;
        job.page_end = p_end;
        job.no_aa = no_aa ? 1 : 0;
        internal::WriteFull(workers[w].cmd_fd, &job, sizeof(job));
        workers[w].in_flight++;
        total_jobs++;
    };

    const auto t0 = std::chrono::high_resolution_clock::now();

    // Stream: dispatch each job the moment it arrives on stdin
    char line[8192];
    while (std::fgets(line, sizeof(line), stdin)) {
        auto len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (std::strncmp(line, "QUIT", 4) == 0) break;
        if (len == 0 || (len == 1 && line[0] == '\n')) continue;

        char* tokens[8];
        const auto ntok = SplitTabs(line, tokens, 8);

        const char* pdf_path;
        const char* out_pattern;
        float dpi = default_dpi;
        int comp = default_compression;

        if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
            pdf_path = tokens[1];
            out_pattern = tokens[2];
            if (ntok >= 4) dpi = static_cast<float>(std::atof(tokens[3]));
            if (ntok >= 6) comp = std::clamp(std::atoi(tokens[5]), -1, 2);
        } else if (ntok >= 2) {
            pdf_path = tokens[0];
            out_pattern = tokens[1];
        } else {
            continue;
        }

        // Drain before dispatching to prevent pipe buffer overflow
        drain_results();

        struct stat st{};
        ::stat(pdf_path, &st);

        if (st.st_size > kLargeFileBytes) {
            auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
            if (doc) {
                const int pages = FPDF_GetPageCount(doc);
                FPDF_CloseDocument(doc);
                const int ranges = std::min(pages, num_workers);
                const int per_range = (pages + ranges - 1) / ranges;
                for (int p = 0; p < pages; p += per_range) {
                    send_job(pick_worker(), pdf_path, out_pattern,
                             dpi, comp, p, std::min(p + per_range, pages));
                }
            } else {
                send_job(pick_worker(), pdf_path, out_pattern, dpi, comp, -1, 0);
            }
        } else {
            send_job(pick_worker(), pdf_path, out_pattern, dpi, comp, -1, 0);
        }
    }

    // Wait for all in-flight jobs to complete
    for (int i = 0; i < num_workers; ++i)
        close(workers[i].cmd_fd);  // signal shutdown

    for (int i = 0; i < num_workers; ++i) {
        PipeResult result;
        while (workers[i].in_flight > 0) {
            if (internal::ReadFull(workers[i].result_fd,
                                   &result, sizeof(result))) {
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

#endif // !_WIN32

// ===========================================================================
// Windows pool mode
// ===========================================================================

#ifdef _WIN32

namespace {

bool ReadFullWin(HANDLE h, void* buf, DWORD count) {
    auto* p = static_cast<uint8_t*>(buf);
    DWORD remaining = count;
    while (remaining > 0) {
        DWORD n = 0;
        if (!::ReadFile(h, p, remaining, &n, nullptr) || n == 0) return false;
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
        if (!::WriteFile(h, p, remaining, &n, nullptr) || n == 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

constexpr DWORD kWinLargeFileBytes = 500 * 1024;

} // anonymous namespace

int RunPoolWorkerWin(HANDLE cmd_h, HANDLE result_h) {
    InitPdfium();
    PipeJob job;
    while (true) {
        if (!ReadFullWin(cmd_h, &job, sizeof(job))) break;

        size_t file_size = 0;
        auto* file_data = internal::ReadFileToMemoryWin(job.pdf_path, file_size);
        FPDF_DOCUMENT doc = nullptr;
        if (file_data)
            doc = FPDF_LoadMemDocument64(file_data, file_size, nullptr);

        PipeResult result{0};
        if (doc) {
            const int p_start = (job.page_start >= 0) ? job.page_start : 0;
            const int p_end = (job.page_start >= 0)
                ? job.page_end : FPDF_GetPageCount(doc);
            for (int p = p_start; p < p_end; ++p) {
                if (internal::RenderPageToFile(doc, p, job.dpi,
                        job.output_pattern, job.compression, job.no_aa))
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

int RunPoolWin(int num_workers, float default_dpi, int default_compression, bool no_aa) {
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
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE cmd_read, cmd_write, result_read, result_write;
        if (!CreatePipe(&cmd_read, &cmd_write, &sa, 0) ||
            !CreatePipe(&result_read, &result_write, &sa, 0)) {
            std::fprintf(stderr, "CreatePipe failed (%lu)\n", GetLastError());
            return 1;
        }
        SetHandleInformation(cmd_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(result_read, HANDLE_FLAG_INHERIT, 0);

        char cmdline[8192];
        std::snprintf(cmdline, sizeof(cmdline),
                      "\"%s\" --pool-worker %llu %llu",
                      exe_path,
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(cmd_read)),
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(result_write)));

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE,
                            0, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n",
                         i, GetLastError());
            CloseHandle(cmd_read); CloseHandle(cmd_write);
            CloseHandle(result_read); CloseHandle(result_write);
            return 1;
        }

        CloseHandle(pi.hThread);
        CloseHandle(cmd_read);
        CloseHandle(result_write);

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
        job.no_aa = no_aa ? 1 : 0;
        WriteFullWin(workers[w].cmd_write, &job, sizeof(job));
        workers[w].in_flight++;
        total_jobs++;
    };

    const auto t0 = std::chrono::high_resolution_clock::now();

    char line[8192];
    while (std::fgets(line, sizeof(line), stdin)) {
        auto len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (std::strncmp(line, "QUIT", 4) == 0) break;
        if (len == 0 || (len == 1 && line[0] == '\n')) continue;

        char* tokens[8];
        const auto ntok = SplitTabs(line, tokens, 8);

        const char* pdf_path;
        const char* out_pattern;
        float dpi = default_dpi;
        int comp = default_compression;

        if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
            pdf_path = tokens[1];
            out_pattern = tokens[2];
            if (ntok >= 4) dpi = static_cast<float>(std::atof(tokens[3]));
            if (ntok >= 6) comp = std::clamp(std::atoi(tokens[5]), -1, 2);
        } else if (ntok >= 2) {
            pdf_path = tokens[0];
            out_pattern = tokens[1];
        } else {
            continue;
        }

        auto pick_w = [&]() -> int {
            int best = 0;
            for (int i = 1; i < num_workers; ++i)
                if (workers[i].in_flight < workers[best].in_flight) best = i;
            return best;
        };

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        GetFileAttributesExA(pdf_path, GetFileExInfoStandard, &fad);
        DWORD fsize = fad.nFileSizeLow;

        if (fsize > kWinLargeFileBytes) {
            auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
            if (doc) {
                const int pages = FPDF_GetPageCount(doc);
                FPDF_CloseDocument(doc);
                const int ranges = std::min(pages, num_workers);
                const int per_range = (pages + ranges - 1) / ranges;
                for (int p = 0; p < pages; p += per_range) {
                    send_job(pick_w(), pdf_path, out_pattern,
                             dpi, comp, p, std::min(p + per_range, pages));
                }
            } else {
                send_job(pick_w(), pdf_path, out_pattern, dpi, comp, -1, 0);
            }
        } else {
            send_job(pick_w(), pdf_path, out_pattern, dpi, comp, -1, 0);
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

#endif // _WIN32

} // namespace fpdf2png::cli
