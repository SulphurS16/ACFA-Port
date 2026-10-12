// Sampling profiler for host threads (diagnostics, --profile <thread-name-substring>[,...]). A sampler
// thread periodically suspends matching threads, records their instruction pointer as an offset
// from the executable's base, and writes "rva count" lines to profile.txt in the run directory
// (symbolize with llvm-addr2line -f -C -e acfa.exe).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rt::prof {

// Registers a host thread (HANDLE) that may be sampled.
void register_thread(const std::string& name, void* handle);
void unregister_thread(void* handle);
// Snapshot of the registered threads (handle, name).
std::vector<std::pair<void*, std::string>> threads();
// Starts sampling the threads whose names contain one of the comma-separated substrings; when
// already sampling, replaces the selection (also control command "profile THREADS").
void start(const std::string& match);
// Clears the samples taken so far ("profile reset").
void reset();
// Writes the samples taken so far and keeps sampling ("profile write NAME").
void write_snapshot(const std::string& path);
// Writes the report and stops sampling (at exit).
void write_report(const std::string& path);
// Logs the CPU time (user + kernel) used so far by each registered thread, busiest first.
void cpu_report();
// The `top` busiest registered threads since the previous call, as "name NN%, ..." (percent of
// one core over the interval).
std::string cpu_interval(int top);

}  // namespace rt::prof
