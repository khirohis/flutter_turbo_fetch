#ifndef FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
#define FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_

#include <windows.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "messages.g.h"
#include "winhttp_helpers.h"
#include "winhttp_transport.h"

namespace flutter_turbo_fetch {

class TurboHttpExecutor {
 public:
  using TransportOverride = std::function<TurboHttpResponse(
      const ValidatedWinHttpRequest&, RequestContext&)>;

  TurboHttpExecutor();
  explicit TurboHttpExecutor(TransportOverride transport_override);
  virtual ~TurboHttpExecutor();

  TurboHttpExecutor(const TurboHttpExecutor&) = delete;
  TurboHttpExecutor& operator=(const TurboHttpExecutor&) = delete;

  virtual void Execute(const TurboHttpRequest& request,
                       std::function<void(TurboHttpResponse)> on_result);

  virtual void Cancel(const std::string& request_id);

  virtual void Shutdown();

  size_t active_request_count() const;
  bool has_active_request(const std::string& request_id) const;

 private:
  class WinThreadPool {
   public:
    WinThreadPool();
    ~WinThreadPool();

    WinThreadPool(const WinThreadPool&) = delete;
    WinThreadPool& operator=(const WinThreadPool&) = delete;

    bool TrySubmit(PTP_SIMPLE_CALLBACK callback, PVOID context);
    void Shutdown();

   private:
    PTP_POOL pool_ = nullptr;
    PTP_CLEANUP_GROUP cleanup_group_ = nullptr;
    TP_CALLBACK_ENVIRON callback_env_{};
    bool env_initialized_ = false;
  };

  struct WorkTask {
    TurboHttpExecutor* executor = nullptr;
    ValidatedWinHttpRequest request;
    std::shared_ptr<RequestContext> ctx;
    std::function<void(TurboHttpResponse)> on_result;
  };

  static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE instance,
                                    PVOID parameter);

  void UnregisterRequestLocked(const std::string& request_id,
                               const std::shared_ptr<RequestContext>& ctx);

  WinHttpTransport transport_;
  TransportOverride transport_override_;
  WinThreadPool thread_pool_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<RequestContext>>
      active_requests_;
  std::unordered_set<std::shared_ptr<RequestContext>> in_flight_contexts_;
  bool is_shutdown_ = false;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_TURBO_HTTP_EXECUTOR_H_
