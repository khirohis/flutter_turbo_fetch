#ifndef FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
#define FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "curl_helpers.h"
#include "curl_transport.h"
#include "turbo_session_manager.h"

namespace flutter_turbo_fetch {

class TurboHttpExecutor {
 public:
  using TransportOverride =
      std::function<void(ValidatedCurlRequest,
                         std::shared_ptr<CurlRequestContext>,
                         std::function<void(TurboHttpResponse)>)>;

  explicit TurboHttpExecutor(TurboSessionManager& session_manager);
  TurboHttpExecutor(TurboSessionManager& session_manager,
                    TransportOverride transport_override);
  virtual ~TurboHttpExecutor();

  TurboHttpExecutor(const TurboHttpExecutor&) = delete;
  TurboHttpExecutor& operator=(const TurboHttpExecutor&) = delete;

  virtual void Execute(const TurboHttpRequest& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void Execute(TurboHttpRequest&& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void CancelRequest(const std::string& request_id);

  virtual void Shutdown();

  size_t active_request_count() const;
  bool has_active_request(const std::string& request_id) const;

 private:
  void ExecuteValidated(ValidatedCurlRequest&& validated,
                        std::function<void(TurboHttpResponse)> on_result);
  void UnregisterRequestLocked(const std::string& request_id,
                               const std::shared_ptr<CurlRequestContext>& ctx);

  CurlMultiTransport transport_;
  TransportOverride transport_override_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<CurlRequestContext>>
      active_requests_;
  // Populated only when a duplicate request_id supersedes an in-flight request,
  // avoiding a second hash-table insert/erase on the common path.
  std::vector<std::shared_ptr<CurlRequestContext>> overflow_contexts_;
  bool is_shutdown_ = false;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
