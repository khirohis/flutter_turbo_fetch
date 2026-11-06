#include "flutter_turbo_fetch_plugin.h"

#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace flutter_turbo_fetch {

// static
void FlutterTurboFetchPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<FlutterTurboFetchPlugin>();
  TurboFetchHostApi::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

FlutterTurboFetchPlugin::FlutterTurboFetchPlugin()
    : executor_(std::make_unique<TurboHttpExecutor>()) {}

FlutterTurboFetchPlugin::FlutterTurboFetchPlugin(
    std::unique_ptr<TurboHttpExecutor> executor)
    : executor_(std::move(executor)) {}

FlutterTurboFetchPlugin::~FlutterTurboFetchPlugin() {
  if (executor_) {
    executor_->Shutdown();
  }
}

void FlutterTurboFetchPlugin::ExecuteRequest(
    const TurboHttpRequest& request,
    std::function<void(ErrorOr<TurboHttpResponse> reply)> result) {
  executor_->Execute(request,
                     [result = std::move(result)](TurboHttpResponse response) {
                       result(ErrorOr<TurboHttpResponse>(std::move(response)));
                     });
}

std::optional<FlutterError> FlutterTurboFetchPlugin::CancelRequest(
    const std::string& request_id) {
  executor_->Cancel(request_id);
  return std::nullopt;
}

}  // namespace flutter_turbo_fetch

