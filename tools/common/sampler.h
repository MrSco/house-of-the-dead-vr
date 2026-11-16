// A sampling profiler for the headless tools on Windows (m2gpushot --sample): a thread stops the thread being
// measured every ~0.2 ms, notes its instruction pointer and lets it go on; at the end the addresses are named
// through the program's debug symbols (build with debug info: RelWithDebInfo) and the commonest functions and
// source lines are printed. Elsewhere it does nothing.
#pragma once

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tools {

class Sampler {
public:
    // Samples the calling thread from now until stop().
    void start() {
        HANDLE self = GetCurrentThread();
        DuplicateHandle(GetCurrentProcess(), self, GetCurrentProcess(), &target_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        run_ = true;
        timeBeginPeriod(1);
        t_ = std::thread([this] {
            while (run_) {
                if (SuspendThread(target_) != DWORD(-1)) {
                    CONTEXT c{};
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(target_, &c)) hits_[c.Rip]++;
                    ResumeThread(target_);
                }
                for (int i = 0; i < 200; i++) YieldProcessor(); // ~0.2 ms between samples with the sleep below
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
        });
    }
    void stop() {
        run_ = false;
        if (t_.joinable()) t_.join();
        timeEndPeriod(1);
    }
    // The commonest functions and lines (top n each), named with the debug symbols.
    void report(int n = 30) const {
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);
        std::map<std::string, uint64_t> funcs, lines;
        uint64_t total = 0;
        for (const auto &[addr, k] : hits_) {
            total += k;
            char buf[sizeof(SYMBOL_INFO) + 512] = {};
            SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 off = 0;
            std::string name = SymFromAddr(proc, addr, &off, sym) ? sym->Name : "?";
            funcs[name] += k;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof line;
            DWORD loff = 0;
            if (SymGetLineFromAddr64(proc, addr, &loff, &line)) {
                std::string f = line.FileName;
                const size_t s = f.find_last_of("\\/");
                lines[(s == std::string::npos ? f : f.substr(s + 1)) + ":" + std::to_string(line.LineNumber)] += k;
            }
        }
        auto print = [&](const char *what, const std::map<std::string, uint64_t> &m) {
            std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
            std::printf("sampler: %llu samples, the commonest %s:\n", (unsigned long long)total, what);
            for (int i = 0; i < n && i < int(v.size()); i++)
                std::printf("  %5.1f%%  %s\n", 100.0 * double(v[size_t(i)].second) / double(total ? total : 1), v[size_t(i)].first.c_str());
        };
        print("functions", funcs);
        print("lines", lines);
        SymCleanup(proc);
    }

private:
    HANDLE target_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread t_;
    std::unordered_map<DWORD64, uint64_t> hits_; // (written by the sampling thread only, read after stop)
};

} // namespace tools

#else
namespace tools {
class Sampler {
public:
    void start() {}
    void stop() {}
    void report(int = 30) const { std::printf("sampler: Windows only\n"); }
};
} // namespace tools
#endif
