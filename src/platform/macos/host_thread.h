// macOS host services used by actual PPU/SPU execution, separate from guest scheduling.
#pragma once
#ifndef __APPLE__
#error "The macOS host-thread backend is only available on Apple platforms"
#endif
#include <cstddef>
#include <cstdint>
#include <string>
#include <pthread.h>

namespace acfa::host {

constexpr std::uint64_t kWaitForever = UINT64_MAX;
std::uint64_t monotonic_time_ns() noexcept;
std::uint64_t monotonic_time_us() noexcept;
// CPU execution time of the calling pthread, in nanoseconds (excludes sleeping/preemption).
std::uint64_t current_thread_cpu_time_ns() noexcept;
void sleep_us(std::uint64_t us) noexcept;
void yield() noexcept;
void set_current_thread_name(const std::string& name) noexcept;

// Manual-reset event. Notification persists until reset(), including notification before wait.
// Finite waits use a monotonic deadline and relative pthread waits, never wall-clock time.
// Owners must join their waiters before destroying the event.
class WakeEvent {
 public:
  explicit WakeEvent(bool signalled = false);
  ~WakeEvent();
  WakeEvent(const WakeEvent&) = delete;
  WakeEvent& operator=(const WakeEvent&) = delete;
  void notify() noexcept;
  void reset() noexcept;
  bool wait_for_us(std::uint64_t timeout_us) noexcept;

 private:
  pthread_mutex_t mutex_{};
  pthread_cond_t condition_{};
  bool signalled_;
};

struct Thread;
// Creates one joinable pthread with an explicitly sized, page-rounded stack and host-page guard.
// On error returns nullptr and the pthread/errno code. No global thread or process policy changes.
Thread* start_thread(void (*entry)(void*), void* argument, std::size_t stack_bytes,
                     const std::string& name, int* error = nullptr);
bool wait_thread(Thread*, std::uint64_t timeout_us) noexcept;
// Successful join/detach consumes the handle. Do not race either operation with another user of it.
// Detached worker bookkeeping is reference-counted and survives until the actual callback returns.
int join_thread(Thread*) noexcept;
int detach_thread(Thread*) noexcept;
std::uint64_t thread_id(Thread*) noexcept;
std::uint64_t current_thread_id() noexcept;

}  // namespace acfa::host
