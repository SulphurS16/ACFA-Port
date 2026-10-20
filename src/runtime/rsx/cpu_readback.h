// Rare CPU reads of GPU-rendered textures. Renderer ownership stays on the FIFO
// thread; no surface traversal, GPU waits or locks are added to ordinary draws.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include "common/bytes.h"
namespace rt::rsx {
class CpuReadbackMailbox {
  enum class Status { Queued, Running, Done, Cancelled };
  struct Request { u32 ea, size; Status status=Status::Queued; bool result=false; };
  std::mutex mutex_;
  std::condition_variable completed_;
  std::deque<std::shared_ptr<Request>> queue_;
  std::atomic<bool> pending_{false};
 public:
  bool pending() const { return pending_.load(std::memory_order_acquire); }
  bool request(u32 ea,u32 size,std::chrono::milliseconds timeout=std::chrono::seconds(5)) {
    auto job=std::make_shared<Request>();job->ea=ea;job->size=size;
    std::unique_lock lock(mutex_);queue_.push_back(job);pending_.store(true,std::memory_order_release);
    if(!completed_.wait_for(lock,timeout,[&]{return job->status==Status::Done;})) {
      if(job->status==Status::Queued) { job->status=Status::Cancelled;return false; }
      // Once the GPU copy starts, do not let the caller reuse/free its memory
      // until that copy is complete. A queued request may be cancelled safely.
      completed_.wait(lock,[&]{return job->status==Status::Done;});
    }
    return job->result;
  }
  template<class F> void service(F readback) {
    if(!pending())return;
    std::shared_ptr<Request> job;
    {
      std::lock_guard lock(mutex_);
      while(!queue_.empty()&&queue_.front()->status==Status::Cancelled)queue_.pop_front();
      if(queue_.empty()){pending_.store(false,std::memory_order_release);return;}
      job=queue_.front();queue_.pop_front();job->status=Status::Running;
      pending_.store(!queue_.empty(),std::memory_order_release);
    }
    const bool result=readback(job->ea,job->size);
    {std::lock_guard lock(mutex_);job->result=result;job->status=Status::Done;}
    completed_.notify_all();
  }
};
}  // namespace rt::rsx
