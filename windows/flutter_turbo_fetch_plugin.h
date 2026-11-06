#ifndef FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_

#include <flutter/plugin_registrar_windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "messages.g.h"
#include "turbo_http_executor.h"

namespace flutter_turbo_fetch {

class FlutterTurboFetchPlugin : public flutter::Plugin,
                                public TurboFetchHostApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

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

 private:
  std::unique_ptr<TurboHttpExecutor> executor_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_H_

