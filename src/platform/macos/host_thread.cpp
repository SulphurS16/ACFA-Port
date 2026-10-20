#include "platform/macos/host_thread.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sched.h>
#include <system_error>
#include <time.h>
#include <unistd.h>

namespace acfa::host {
namespace {

std::uint64_t deadline_ns(std::uint64_t us) noexcept {
  const auto now = monotonic_time_ns();
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  return us > (max - now) / 1000 ? max : now + us * 1000;
}

timespec relative_time(std::uint64_t ns) noexcept {
  // A one-second quantum bounds conversion and allows deadline rechecks after interruptions.
  ns = std::min<std::uint64_t>(ns, 1000000000);
  return {static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
}

}  // namespace

std::uint64_t monotonic_time_ns() noexcept {
  timespec t{};
  if (clock_gettime(CLOCK_MONOTONIC, &t)) std::abort();
  return std::uint64_t(t.tv_sec) * 1000000000 + std::uint64_t(t.tv_nsec);
}

std::uint64_t monotonic_time_us() noexcept { return monotonic_time_ns() / 1000; }

std::uint64_t current_thread_cpu_time_ns() noexcept {
  timespec t{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) std::abort();
  return std::uint64_t(t.tv_sec) * 1000000000 + std::uint64_t(t.tv_nsec);
}

void yield() noexcept { sched_yield(); }

void set_current_thread_name(const std::string& name) noexcept {
  auto n = std::min<std::size_t>(name.size(), 63);
  while (n < name.size() && n && (static_cast<unsigned char>(name[n]) & 0xc0) == 0x80) --n;
  char native_name[64]{};
  std::memcpy(native_name, name.data(), n);
  pthread_setname_np(native_name);
}

void sleep_us(std::uint64_t us) noexcept {
  if (!us) {
    yield();
    return;
  }
  const auto deadline = deadline_ns(us);
  for (;;) {
    const auto now = monotonic_time_ns();
    if (now >= deadline) return;
    const auto remaining = relative_time(deadline - now);
    if (nanosleep(&remaining, nullptr) && errno != EINTR) return;
  }
}

WakeEvent::WakeEvent(bool signalled) : signalled_(signalled) {
  int e = pthread_mutex_init(&mutex_, nullptr);
  if (e) throw std::system_error(e, std::generic_category(), "pthread_mutex_init");
  e = pthread_cond_init(&condition_, nullptr);
  if (e) {
    pthread_mutex_destroy(&mutex_);
    throw std::system_error(e, std::generic_category(), "pthread_cond_init");
  }
}

WakeEvent::~WakeEvent() {
  pthread_cond_destroy(&condition_);
  pthread_mutex_destroy(&mutex_);
}

void WakeEvent::notify() noexcept {
  pthread_mutex_lock(&mutex_);
  signalled_ = true;
  pthread_cond_broadcast(&condition_);
  pthread_mutex_unlock(&mutex_);
}

void WakeEvent::reset() noexcept {
  pthread_mutex_lock(&mutex_);
  signalled_ = false;
  pthread_mutex_unlock(&mutex_);
}

bool WakeEvent::wait_for_us(std::uint64_t timeout_us) noexcept {
  const auto deadline = timeout_us == kWaitForever ? kWaitForever : deadline_ns(timeout_us);
  pthread_mutex_lock(&mutex_);
  while (!signalled_) {
    if (timeout_us == kWaitForever) {
      if (pthread_cond_wait(&condition_, &mutex_)) break;
    } else {
      const auto now = monotonic_time_ns();
      if (now >= deadline) break;
      const auto relative = relative_time(deadline - now);
      const auto e = pthread_cond_timedwait_relative_np(&condition_, &mutex_, &relative);
      if (e && e != ETIMEDOUT && e != EINTR) break;
    }
  }
  const bool signalled = signalled_;
  pthread_mutex_unlock(&mutex_);
  return signalled;
}

struct Thread {
  pthread_t native{};
  void (*entry)(void*) = nullptr;
  void* argument = nullptr;
  std::string name;
  std::atomic<unsigned> refs{2};  // caller handle plus live worker
  WakeEvent done;
};

namespace {
void release(Thread* thread) noexcept {
  if (thread->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete thread;
}

void* thread_entry(void* argument) {
  auto* t = static_cast<Thread*>(argument);
  set_current_thread_name(t->name);
  t->entry(t->argument);
  t->done.notify();
  release(t);
  return nullptr;
}
}  // namespace

Thread* start_thread(void (*entry)(void*), void* argument, std::size_t stack_bytes,
                     const std::string& name, int* error) {
  const long page = sysconf(_SC_PAGESIZE);
  if (error) *error = 0;
  if (!entry || page <= 0 || stack_bytes < PTHREAD_STACK_MIN ||
      stack_bytes > std::numeric_limits<std::size_t>::max() - std::size_t(page - 1)) {
    if (error) *error = EINVAL;
    return nullptr;
  }
  stack_bytes = ((stack_bytes + std::size_t(page - 1)) / std::size_t(page)) * std::size_t(page);
  Thread* t = nullptr;
  try {
    t = new Thread;
    t->entry = entry;
    t->argument = argument;
    t->name = name;
  } catch (const std::system_error& e) {
    delete t;
    if (error) *error = e.code().value();
    return nullptr;
  } catch (const std::bad_alloc&) {
    delete t;
    if (error) *error = ENOMEM;
    return nullptr;
  }
  pthread_attr_t attr;
  int e = pthread_attr_init(&attr);
  const bool attr_initialized = !e;
  if (!e) e = pthread_attr_setstacksize(&attr, stack_bytes);
  if (!e) e = pthread_attr_setguardsize(&attr, std::size_t(page));
  if (!e) e = pthread_create(&t->native, &attr, thread_entry, t);
  if (attr_initialized) pthread_attr_destroy(&attr);
  if (e) {
    delete t;
    if (error) *error = e;
    return nullptr;
  }
  return t;
}

bool wait_thread(Thread* t, std::uint64_t timeout_us) noexcept {
  return t && t->done.wait_for_us(timeout_us);
}

int join_thread(Thread* t) noexcept {
  if (!t) return EINVAL;
  if (pthread_equal(t->native, pthread_self())) return EDEADLK;
  const int e = pthread_join(t->native, nullptr);
  if (!e) release(t);
  return e;
}

int detach_thread(Thread* t) noexcept {
  if (!t) return EINVAL;
  const int e = pthread_detach(t->native);
  if (!e) release(t);
  return e;
}

std::uint64_t thread_id(Thread* t) noexcept {
  std::uint64_t id = 0;
  if (t) pthread_threadid_np(t->native, &id);
  return id;
}

std::uint64_t current_thread_id() noexcept {
  std::uint64_t id = 0;
  pthread_threadid_np(nullptr, &id);
  return id;
}

}  // namespace acfa::host
