#include "turbo_http_executor.h"

#include <exception>
#include <utility>
#include <vector>

namespace flutter_turbo_fetch {

TurboHttpExecutor::WinThreadPool::WinThreadPool() {
  pool_ = CreateThreadpool(nullptr);
  if (pool_ != nullptr) {
    SetThreadpoolThreadMaximum(pool_, 64);
    SetThreadpoolThreadMinimum(pool_, 1);
  }
  cleanup_group_ = CreateThreadpoolCleanupGroup();
  InitializeThreadpoolEnvironment(&callback_env_);
  env_initialized_ = true;
  if (pool_ != nullptr) {
    SetThreadpoolCallbackPool(&callback_env_, pool_);
  }
  if (cleanup_group_ != nullptr) {
    SetThreadpoolCallbackCleanupGroup(&callback_env_, cleanup_group_, nullptr);
  }
}

TurboHttpExecutor::WinThreadPool::~WinThreadPool() { Shutdown(); }

bool TurboHttpExecutor::WinThreadPool::TrySubmit(PTP_SIMPLE_CALLBACK callback,
                                                 PVOID context) {
  if (cleanup_group_ == nullptr || !env_initialized_) {
    return false;
  }
  return TrySubmitThreadpoolCallback(callback, context, &callback_env_) !=
         FALSE;
}

void TurboHttpExecutor::WinThreadPool::Shutdown() {
  if (cleanup_group_ != nullptr) {
    CloseThreadpoolCleanupGroupMembers(cleanup_group_, TRUE, nullptr);
    CloseThreadpoolCleanupGroup(cleanup_group_);
    cleanup_group_ = nullptr;
  }
  if (env_initialized_) {
    DestroyThreadpoolEnvironment(&callback_env_);
    env_initialized_ = false;
  }
  if (pool_ != nullptr) {
    CloseThreadpool(pool_);
    pool_ = nullptr;
  }
}

TurboHttpExecutor::TurboHttpExecutor() = default;

TurboHttpExecutor::TurboHttpExecutor(TransportOverride transport_override)
    : transport_override_(std::move(transport_override)) {}

TurboHttpExecutor::~TurboHttpExecutor() { Shutdown(); }

void TurboHttpExecutor::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  // 1. Synchronous pre-dispatch validation & normalization
  ValidatedWinHttpRequest validated;
  std::string validation_error;
  if (!ValidateWinHttpRequest(request, &validated, &validation_error)) {
    on_result(MakeErrorResponse("connectionError", validation_error));
    return;
  }

  // 2. Build task before locking, then atomically register and submit under
  // mutex_ so Shutdown() cannot destroy the threadpool mid-submission.
  auto task = std::make_unique<WorkTask>(WorkTask{
      this,
      std::move(validated),
      std::make_shared<RequestContext>(),
      std::move(on_result)});

  bool shut_down = false;
  bool submitted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      shut_down = true;
    } else {
      active_requests_[task->request.request_id] = task->ctx;
      in_flight_contexts_.insert(task->ctx);
      submitted =
          thread_pool_.TrySubmit(&TurboHttpExecutor::WorkCallback, task.get());
      if (submitted) {
        task.release();
      } else {
        UnregisterRequestLocked(task->request.request_id, task->ctx);
      }
    }
  }

  if (shut_down) {
    task->on_result(MakeErrorResponse("cancel", "Executor is shut down."));
  } else if (!submitted) {
    task->on_result(
        MakeErrorResponse("unknown", "Failed to submit threadpool callback."));
  }
}

// static
void CALLBACK TurboHttpExecutor::WorkCallback(
    PTP_CALLBACK_INSTANCE /*instance*/,
    PVOID parameter) {
  std::unique_ptr<WorkTask> task(static_cast<WorkTask*>(parameter));
  TurboHttpExecutor* self = task->executor;
  TurboHttpResponse response;

  try {
    if (task->ctx->is_cancelled.load()) {
      response = MakeCancelResponse();
    } else if (self->transport_override_) {
      response = self->transport_override_(task->request, *task->ctx);
    } else {
      response = self->transport_.Execute(task->request, *task->ctx);
    }
  } catch (const std::exception& e) {
    response = MakeErrorResponse("unknown", e.what());
  } catch (...) {
    response = MakeErrorResponse("unknown",
                                 "Unexpected native runtime exception.");
  }

  task->ctx->CloseRequestHandle();
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->UnregisterRequestLocked(task->request.request_id, task->ctx);
  }

  task->on_result(std::move(response));
}

void TurboHttpExecutor::UnregisterRequestLocked(
    const std::string& request_id,
    const std::shared_ptr<RequestContext>& ctx) {
  in_flight_contexts_.erase(ctx);
  auto it = active_requests_.find(request_id);
  if (it != active_requests_.end() && it->second == ctx) {
    active_requests_.erase(it);
  }
}

void TurboHttpExecutor::Cancel(const std::string& request_id) {
  std::shared_ptr<RequestContext> ctx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_requests_.find(request_id);
    if (it != active_requests_.end()) {
      ctx = it->second;
      active_requests_.erase(it);
    }
  }
  if (ctx != nullptr) {
    ctx->Cancel();
  }
}

void TurboHttpExecutor::Shutdown() {
  std::vector<std::shared_ptr<RequestContext>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      return;
    }
    is_shutdown_ = true;
    to_cancel.reserve(in_flight_contexts_.size());
    for (const auto& ctx : in_flight_contexts_) {
      to_cancel.push_back(ctx);
    }
    in_flight_contexts_.clear();
    active_requests_.clear();
  }

  for (auto& ctx : to_cancel) {
    ctx->Cancel();
  }

  thread_pool_.Shutdown();
  transport_.Shutdown();
}

size_t TurboHttpExecutor::active_request_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.size();
}

bool TurboHttpExecutor::has_active_request(
    const std::string& request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.find(request_id) != active_requests_.end();
}

}  // namespace flutter_turbo_fetch
