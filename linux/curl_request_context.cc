#include "curl_request_context.h"

#include <algorithm>
#include <climits>

namespace flutter_turbo_fetch {

namespace {

constexpr size_t kInitialUnknownBodyBytes = 8 * 1024;
constexpr size_t kInitialHeaderBuckets = 16;
constexpr int64_t kMaxPreallocBytes = 64 * 1024 * 1024;

}  // namespace

void CurlRequestContext::RecordCallbackException(std::string_view message) {
  callback_exception_message.assign(message.data(), message.size());
  callback_exception.store(true, std::memory_order_relaxed);
}

void CurlRequestContext::InitializeFromRequest(
    const ValidatedCurlRequest& request,
    std::chrono::steady_clock::time_point now) {
  is_head_request = EqualsIgnoreCaseAscii(request.method, "HEAD");
  has_request_body = request.body.has_value() && !request.body->empty();
  follow_redirects = request.follow_redirects;
  response_headers.reserve(kInitialHeaderBuckets);

  if (request.connect_timeout_ms.has_value() &&
      *request.connect_timeout_ms > 0) {
    connect_timeout_ms = *request.connect_timeout_ms;
  }
  if (request.send_timeout_ms.has_value() && *request.send_timeout_ms > 0) {
    send_timeout_ms = *request.send_timeout_ms;
  }
  if (request.receive_timeout_ms.has_value() &&
      *request.receive_timeout_ms > 0) {
    receive_timeout_ms = *request.receive_timeout_ms;
  }
  if (request.max_response_bytes.has_value()) {
    max_response_bytes = std::max<int64_t>(0, *request.max_response_bytes);
  }

  phase = ExecutionPhase::kConnecting;
  start_time = now;
  last_send_activity = now;
  last_receive_activity = now;
  last_ulnow = 0;
  last_dlnow = 0;
}

bool CurlRequestContext::OnHeaderLine(
    std::string_view line,
    std::chrono::steady_clock::time_point now) {
  if (is_cancelled.load(std::memory_order_relaxed) ||
      is_exceeded_limit.load(std::memory_order_relaxed)) {
    return false;
  }

  ParsedStatusLine parsed_status;
  if (ParseStatusLine(line, &parsed_status)) {
    current_status_code = parsed_status.status_code;
    has_location_header = false;
    response_headers.clear();
    content_length = std::nullopt;
    if (IsInformationalResponse()) {
      // Do not advance phase out of kConnecting/kSendingBody on 1xx responses
      // (e.g. 100 Continue or 103 Early Hints).
      return true;
    }

    // Reset state on every non-1xx status line so redirect intermediates are dropped.
    phase = ExecutionPhase::kReceiving;
    if (receive_timeout_ms.has_value()) {
      last_receive_activity = now;
    }
    response_body.clear();
    body_reserved = false;
    status_message = std::move(parsed_status.status_message);
    return true;
  }

  if (IsInformationalResponse()) {
    return true;
  }

  phase = ExecutionPhase::kReceiving;
  if (receive_timeout_ms.has_value()) {
    last_receive_activity = now;
  }

  std::string key;
  std::string_view val_view;
  if (!ParseHeaderLine(line, &key, &val_view)) {
    return true;
  }

  if (key == "location" && !val_view.empty()) {
    has_location_header = true;
  }

  if (key == "content-length") {
    std::optional<int64_t> parsed_len = ParseContentLength(val_view);
    content_length = parsed_len;

    // Defer Content-Length limit enforcement on redirectable 3xx responses
    // because Location may arrive on a subsequent header line; if no Location
    // is present, OnBodyChunk() enforces maxResponseBytes on the actual body.
    if (!ShouldDeferContentLengthLimit() && !is_head_request &&
        parsed_len.has_value()) {
      if (max_response_bytes.has_value() &&
          *parsed_len > *max_response_bytes) {
        exceeded_observed_bytes = *parsed_len;
        is_exceeded_limit.store(true, std::memory_order_relaxed);
        return false;
      }
      if (!body_reserved && *parsed_len > 0) {
        int64_t reserve_bytes =
            std::min<int64_t>(*parsed_len, kMaxPreallocBytes);
        if (max_response_bytes.has_value()) {
          reserve_bytes = std::min<int64_t>(reserve_bytes, *max_response_bytes);
        }
        if (reserve_bytes > 0) {
          response_body.reserve(static_cast<size_t>(reserve_bytes));
          body_reserved = true;
        }
      }
    }
  }

  response_headers[std::move(key)].emplace_back(val_view.data(),
                                                val_view.size());
  return true;
}

bool CurlRequestContext::OnBodyChunk(
    const uint8_t* data,
    size_t len,
    std::chrono::steady_clock::time_point now) {
  if (is_cancelled.load(std::memory_order_relaxed) ||
      is_exceeded_limit.load(std::memory_order_relaxed)) {
    return false;
  }

  if (IsRedirectIntermediate()) {
    // libcurl delivers 3xx response bodies even when FOLLOWLOCATION is enabled;
    // discard them so they neither corrupt the final body nor trigger maxBytes.
    if (receive_timeout_ms.has_value()) {
      last_receive_activity = now;
    }
    return true;
  }

  phase = ExecutionPhase::kReceiving;
  if (receive_timeout_ms.has_value()) {
    last_receive_activity = now;
  }

  if (len == 0) {
    return true;
  }

  if (max_response_bytes.has_value()) {
    const uint64_t limit = static_cast<uint64_t>(*max_response_bytes);
    const uint64_t current = static_cast<uint64_t>(response_body.size());
    if (current > limit || static_cast<uint64_t>(len) > limit - current) {
      is_exceeded_limit.store(true, std::memory_order_relaxed);
      response_body.clear();
      response_body.shrink_to_fit();
      return false;
    }
  }

  if (!body_reserved) {
    size_t initial = kInitialUnknownBodyBytes;
    if (max_response_bytes.has_value()) {
      const uint64_t capped_limit = std::min<uint64_t>(
          static_cast<uint64_t>(*max_response_bytes),
          static_cast<uint64_t>(kMaxPreallocBytes));
      initial = std::min<size_t>(initial, static_cast<size_t>(capped_limit));
    }
    initial = std::max<size_t>(initial, len);
    response_body.reserve(initial);
    body_reserved = true;
  }

  response_body.insert(response_body.end(), data, data + len);
  return true;
}

bool CurlRequestContext::OnProgress(curl_off_t dlnow,
                                    curl_off_t ulnow,
                                    curl_off_t ultotal,
                                    bool connect_completed,
                                    std::chrono::steady_clock::time_point now) {
  if (is_cancelled.load(std::memory_order_relaxed) ||
      is_exceeded_limit.load(std::memory_order_relaxed)) {
    return false;
  }

  if (phase == ExecutionPhase::kConnecting) {
    if (dlnow > 0) {
      phase = ExecutionPhase::kReceiving;
      last_receive_activity = now;
    } else if (ulnow > 0 || connect_completed) {
      const bool upload_finished = (ultotal > 0 && ulnow >= ultotal);
      if (has_request_body && !upload_finished) {
        phase = ExecutionPhase::kSendingBody;
        last_send_activity = now;
      } else {
        phase = ExecutionPhase::kReceiving;
        last_receive_activity = now;
      }
    }
  } else if (phase == ExecutionPhase::kSendingBody) {
    if (ulnow > last_ulnow) {
      last_send_activity = now;
    }
    if (dlnow > 0 || (ultotal > 0 && ulnow >= ultotal)) {
      phase = ExecutionPhase::kReceiving;
      last_receive_activity = now;
    }
  } else if (phase == ExecutionPhase::kReceiving) {
    if (dlnow > last_dlnow) {
      last_receive_activity = now;
    }
  }

  last_ulnow = ulnow;
  last_dlnow = dlnow;

  return !CheckPhaseTimeout(now);
}

std::optional<CurlRequestContext::ActiveTimeout>
CurlRequestContext::ResolveActiveTimeout() const {
  std::optional<int64_t> limit;
  std::chrono::steady_clock::time_point origin = start_time;
  const char* error_code = nullptr;

  switch (phase) {
    case ExecutionPhase::kConnecting:
      limit = connect_timeout_ms;
      origin = start_time;
      error_code = TurboErrorCodes::kConnectionTimeout;
      break;
    case ExecutionPhase::kSendingBody:
      limit = send_timeout_ms;
      origin = last_send_activity;
      error_code = TurboErrorCodes::kSendTimeout;
      break;
    case ExecutionPhase::kReceiving:
      limit = receive_timeout_ms;
      origin = last_receive_activity;
      error_code = TurboErrorCodes::kReceiveTimeout;
      break;
  }

  if (!limit.has_value() || *limit <= 0) {
    return std::nullopt;
  }
  return ActiveTimeout{*limit, origin, error_code};
}

bool CurlRequestContext::CheckPhaseTimeout(
    std::chrono::steady_clock::time_point now) {
  if (timed_out_error_code != nullptr) {
    return true;
  }

  auto active = ResolveActiveTimeout();
  if (!active.has_value()) {
    return false;
  }

  const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - active->origin)
                              .count();
  if (elapsed >= active->limit_ms) {
    timed_out_error_code = active->error_code;
    return true;
  }
  return false;
}

std::optional<int> CurlRequestContext::RemainingTimeoutMs(
    std::chrono::steady_clock::time_point now) const {
  auto active = ResolveActiveTimeout();
  if (!active.has_value()) {
    return std::nullopt;
  }

  const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - active->origin)
                              .count();
  const int64_t remaining = std::max<int64_t>(0, active->limit_ms - elapsed);
  return (remaining > static_cast<int64_t>(INT_MAX))
             ? INT_MAX
             : static_cast<int>(remaining);
}

}  // namespace flutter_turbo_fetch
