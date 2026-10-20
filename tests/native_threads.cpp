// Real pthread/time/event acceptance test; no game assets or desktop interaction.
#include "platform/macos/host_thread.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <pthread.h>

namespace host = acfa::host;

namespace {
void require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

struct Worker {
  host::WakeEvent ready, stop;
  std::atomic<unsigned> count{0};
  std::atomic<std::uint64_t> id{0};
  std::size_t stack_size = 0;
  char name[64]{};
};

void worker(void* argument) {
  auto& s = *static_cast<Worker*>(argument);
  s.id = host::current_thread_id();
  s.stack_size = pthread_get_stacksize_np(pthread_self());
  pthread_getname_np(pthread_self(), s.name, sizeof(s.name));
  ++s.count;
  s.ready.notify();
  s.stop.wait_for_us(host::kWaitForever);
  ++s.count;
}

struct SelfJoin {
  host::WakeEvent ready;
  host::Thread* thread = nullptr;
  std::atomic<int> result{-1};
};
void self_join(void* argument) {
  auto& s = *static_cast<SelfJoin*>(argument);
  s.ready.wait_for_us(host::kWaitForever);
  s.result = host::join_thread(s.thread);
}

void increment(void* p) { ++*static_cast<std::atomic<unsigned>*>(p); }

struct Detached {
  host::WakeEvent release;
  std::atomic<unsigned> done{0};
};
void detached(void* p) {
  auto& s = *static_cast<Detached*>(p);
  s.release.wait_for_us(host::kWaitForever);
  ++s.done;  // Last access to caller's storage; the wrapper owns its remaining bookkeeping.
}
}  // namespace

int main() {
  const auto initial = host::monotonic_time_ns();
  auto last = initial;
  for (int n = 0; n < 100000; ++n) {
    const auto now = host::monotonic_time_ns();
    require(now >= last, "monotonic clock cannot run backwards");
    last = now;
  }
  require(host::monotonic_time_us() >= last / 1000, "clock units agree");
  const auto sleep_start = host::monotonic_time_ns();
  host::sleep_us(10000);
  const auto slept = host::monotonic_time_ns() - sleep_start;
  require(slept >= 10000000 && slept < 1000000000, "10ms sleep is bounded and not early");
  host::sleep_us(0);
  const auto cpu0 = host::current_thread_cpu_time_ns();
  const auto wall0 = host::monotonic_time_ns();
  host::sleep_us(10000);
  const auto cpu_sleep = host::current_thread_cpu_time_ns() - cpu0;
  const auto wall_sleep = host::monotonic_time_ns() - wall0;
  require(cpu_sleep < wall_sleep / 2, "thread CPU time excludes actual sleeping");
  std::uint64_t busy_value = 1;
  const auto busy0 = host::current_thread_cpu_time_ns();
  do {
    for (unsigned n = 0; n < 1024; ++n) busy_value = busy_value * 1664525 + 1013904223;
  } while (host::current_thread_cpu_time_ns() - busy0 < 2000000);
  require(busy_value != 1, "CPU measurement loop executes work");
  require(host::current_thread_cpu_time_ns() > busy0, "actual work increases native CPU ns");

  host::WakeEvent event;
  auto start = host::monotonic_time_ns();
  require(!event.wait_for_us(5000), "unset event times out");
  require(host::monotonic_time_ns() - start >= 5000000, "event does not time out early");
  event.notify();
  require(event.wait_for_us(0) && event.wait_for_us(0), "manual event preserves notification");
  event.reset();
  require(!event.wait_for_us(0), "reset clears notification");
  int error = 0;
  require(!host::start_thread(nullptr, nullptr, 1 << 20, "bad", &error) && error == EINVAL, "null entry rejected");
  require(!host::start_thread(increment, nullptr, 1, "bad", &error) && error == EINVAL, "undersized stack rejected");
  require(!host::start_thread(increment, nullptr, std::numeric_limits<std::size_t>::max(), "bad", &error) &&
              error == EINVAL, "stack alignment overflow rejected");
  require(host::join_thread(nullptr) == EINVAL && host::detach_thread(nullptr) == EINVAL, "null handles rejected");

  constexpr std::size_t stack = 16u << 20;
  Worker state;
  auto* thread = host::start_thread(worker, &state, stack, "acfa:ppu-native-test", &error);
  require(thread && !error, "real pthread creation");
  require(state.ready.wait_for_us(1000000), "worker starts within timeout");
  require(state.id && state.id != host::current_thread_id(), "worker has independent native id");
  require(host::thread_id(thread) == state.id, "host id agrees with worker");
  require(state.stack_size >= stack, "worker receives explicit 16MiB stack");
  require(!std::strcmp(state.name, "acfa:ppu-native-test"), "native thread name set from worker");
  require(!host::wait_thread(thread, 5000), "live stop waiter does not appear completed");
  state.stop.notify();
  require(host::wait_thread(thread, 1000000), "stop notification wakes actual worker");
  require(!host::join_thread(thread) && state.count == 2, "actual pthread joined and final writes visible");

  Worker ppu_stack;
  auto* large = host::start_thread(worker, &ppu_stack, 64u << 20, "acfa:ppu-64MiB", &error);
  require(large && ppu_stack.ready.wait_for_us(1000000), "real 64MiB PPU host stack starts");
  require(ppu_stack.stack_size >= (64u << 20), "PPU host stack retains requested 64MiB");
  ppu_stack.stop.notify();
  require(host::wait_thread(large, 1000000) && !host::join_thread(large), "64MiB worker stops and joins");

  SelfJoin self;
  self.thread = host::start_thread(self_join, &self, 1u << 20, "acfa:self-join", &error);
  require(self.thread, "self-join worker creation");
  self.ready.notify();
  require(host::wait_thread(self.thread, 1000000) && !host::join_thread(self.thread) && self.result == EDEADLK,
          "self join rejected without deadlock");

  // Repeated joins exercise worker-finishes-before-creator and parent-waits-first races.
  std::atomic<unsigned> joined{0};
  for (unsigned n = 0; n < 64; ++n) {
    auto* t = host::start_thread(increment, &joined, 1u << 20, "acfa:join-race", &error);
    require(t && host::wait_thread(t, 1000000) && !host::join_thread(t), "repeated actual join");
  }
  require(joined == 64, "all repeated workers executed exactly once");

  Detached det;
  for (unsigned n = 0; n < 64; ++n) {
    auto* t = host::start_thread(detached, &det, 1u << 20, "acfa:detached", &error);
    require(t && !host::detach_thread(t), "live worker detaches successfully");
  }
  det.release.notify();
  const auto detach_deadline = host::monotonic_time_us() + 1000000;
  while (det.done != 64 && host::monotonic_time_us() < detach_deadline) host::sleep_us(1000);
  require(det.done == 64, "detached workers survive consumed handles and wake in broadcast");
  std::printf("PASS native threads: 100000 monotonic checks, sleep %.3fms, stack %zu, real 16/64MiB stacks, join/self-join/64 joins/64 detaches/stop broadcast/CPU ns\n",
              double(slept) / 1e6, state.stack_size);
  return 0;
}
