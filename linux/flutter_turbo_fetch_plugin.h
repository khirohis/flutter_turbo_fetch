#ifndef FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_
#define FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_

#include <flutter/binary_messenger.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "messages.g.h"
#include "turbo_http_executor.h"

namespace flutter_turbo_fetch {

class FlutterTurboFetchPlugin : public TurboFetchHostApi {
 public:
  static void RegisterWithMessenger(flutter::BinaryMessenger* messenger);

  FlutterTurboFetchPlugin();
  explicit FlutterTurboFetchPlugin(std::unique_ptr<TurboHttpExecutor> executor);
  ~FlutterTurboFetchPlugin() override;

  FlutterTurboFetchPlugin(const FlutterTurboFetchPlugin&) = delete;
  FlutterTurboFetchPlugin& operator=(const FlutterTurboFetchPlugin&) = delete;

  // TurboFetchHostApi implementation.
  void ExecuteRequest(
      const TurboHttpRequest& request,
      std::function<void(ErrorOr<TurboHttpResponse> reply)> result) override;

  std::optional<FlutterError> CancelRequest(
      const std::string& request_id) override;

  // Public convenience delegation helper used in unit tests
  void Execute(const TurboHttpRequest& request,
               std::function<void(TurboHttpResponse)> on_result);

 private:
  std::unique_ptr<TurboSessionManager> owned_session_manager_;
  std::unique_ptr<TurboHttpExecutor> executor_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_

