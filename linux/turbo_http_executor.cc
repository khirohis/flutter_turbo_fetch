#include "turbo_http_executor.h"

#include <exception>
#include <utility>
#include <vector>

namespace flutter_turbo_fetch {

TurboHttpExecutor::TurboHttpExecutor(TurboSessionManager& session_manager)
    : transport_(session_manager) {}

TurboHttpExecutor::TurboHttpExecutor(TurboSessionManager& session_manager,
                                     TransportOverride transport_override)
    : transport_(session_manager),
      transport_override_(std::move(transport_override)) {}

TurboHttpExecutor::~TurboHttpExecutor() { Shutdown(); }

size_t TurboHttpExecutor::active_request_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.size() + overflow_contexts_.size();
}

bool TurboHttpExecutor::has_active_request(
    const std::string& request_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_requests_.find(request_id) != active_requests_.end();
}

void TurboHttpExecutor::UnregisterRequestLocked(
    const std::string& request_id,
    const std::shared_ptr<CurlRequestContext>& ctx) {
  auto it = active_requests_.find(request_id);
  if (it != active_requests_.end() && it->second == ctx) {
    active_requests_.erase(it);
    return;
  }

  for (size_t i = 0; i < overflow_contexts_.size(); ++i) {
    if (overflow_contexts_[i] == ctx) {
      overflow_contexts_[i] = std::move(overflow_contexts_.back());
      overflow_contexts_.pop_back();
      return;
    }
  }
}

void TurboHttpExecutor::CancelRequest(const std::string& request_id) {
  std::shared_ptr<CurlRequestContext> ctx;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_requests_.find(request_id);
    if (it != active_requests_.end()) {
      ctx = it->second;
    }
  }

  if (ctx != nullptr) {
    ctx->is_cancelled.store(true, std::memory_order_release);
    if (!transport_override_) {
      transport_.WakeupForCancel();
    }
  }
}

void TurboHttpExecutor::Shutdown() {
  std::vector<std::shared_ptr<CurlRequestContext>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      return;
    }
    is_shutdown_ = true;
    to_cancel.reserve(active_requests_.size() + overflow_contexts_.size());
    for (auto& [id, ctx] : active_requests_) {
      (void)id;
      to_cancel.push_back(std::move(ctx));
    }
    for (auto& ctx : overflow_contexts_) {
      to_cancel.push_back(std::move(ctx));
    }
    active_requests_.clear();
    overflow_contexts_.clear();
  }

  for (auto& ctx : to_cancel) {
    ctx->is_cancelled.store(true, std::memory_order_release);
  }

  if (!transport_override_) {
    transport_.Shutdown();
  }
}

void TurboHttpExecutor::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  ValidatedCurlRequest validated;
  std::string validation_error;
  if (!ValidateCurlRequest(request, &validated, &validation_error)) {
    if (on_result) {
      on_result(
          MakeErrorResponse(TurboErrorCodes::kConnectionError, validation_error));
    }
    return;
  }
  ExecuteValidated(std::move(validated), std::move(on_result));
}

void TurboHttpExecutor::Execute(
    TurboHttpRequest&& request,
    std::function<void(TurboHttpResponse)> on_result) {
  ValidatedCurlRequest validated;
  std::string validation_error;
  if (!ValidateCurlRequest(std::move(request), &validated, &validation_error)) {
    if (on_result) {
      on_result(
          MakeErrorResponse(TurboErrorCodes::kConnectionError, validation_error));
    }
    return;
  }
  ExecuteValidated(std::move(validated), std::move(on_result));
}

void TurboHttpExecutor::ExecuteValidated(
    ValidatedCurlRequest&& validated,
    std::function<void(TurboHttpResponse)> on_result) {
  auto ctx = std::make_shared<CurlRequestContext>();
  ctx->on_result = std::move(on_result);
  const std::string request_id = validated.request_id;

  auto complete_once = [this, request_id, ctx](TurboHttpResponse response) {
    if (!ctx->TryMarkCompleted()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      UnregisterRequestLocked(request_id, ctx);
    }
    auto cb = std::move(ctx->on_result);
    if (cb) {
      cb(std::move(response));
    }
  };

  // Register synchronously and, for the real transport, enqueue atomically
  // under mutex_ so Shutdown() cannot complete transport shutdown while a new
  // request is still mid-submission.
  bool shut_down = false;
  bool should_wakeup = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      shut_down = true;
    } else {
      auto [it, inserted] = active_requests_.emplace(request_id, ctx);
      if (!inserted) {
        overflow_contexts_.push_back(std::move(it->second));
        it->second = ctx;
      }
      if (!transport_override_) {
        if (!transport_.Enqueue(std::move(validated), ctx,
                                std::move(complete_once), &should_wakeup)) {
          UnregisterRequestLocked(request_id, ctx);
          shut_down = true;
        }
      }
    }
  }

  if (shut_down) {
    auto cb = std::move(ctx->on_result);
    if (cb) {
      cb(MakeErrorResponse(TurboErrorCodes::kCancel, "Executor is shut down."));
    }
    return;
  }

  if (!transport_override_) {
    if (should_wakeup) {
      transport_.Wakeup();
    }
    return;
  }

  auto fail_transport_override = [&](std::string_view message) {
    if (ctx->TryMarkCompleted()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        UnregisterRequestLocked(request_id, ctx);
      }
      auto cb = std::move(ctx->on_result);
      if (cb) {
        cb(MakeErrorResponse(TurboErrorCodes::kUnknown, message));
      }
    }
  };

  try {
    transport_override_(std::move(validated), ctx, std::move(complete_once));
  } catch (const std::exception& e) {
    fail_transport_override(e.what());
  } catch (...) {
    fail_transport_override("Unexpected native runtime exception.");
  }
}

}  // namespace flutter_turbo_fetch
