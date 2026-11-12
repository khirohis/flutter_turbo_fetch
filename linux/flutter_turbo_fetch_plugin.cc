#include "flutter_turbo_fetch_plugin.h"

#ifndef TURBO_STANDALONE_TEST
#include "include/flutter_turbo_fetch/flutter_turbo_fetch_plugin.h"
#include <flutter_linux/flutter_linux.h>

#include <memory>
#include <utility>

struct _FlutterTurboFetchPlugin {
  GObject parent_instance;
  std::unique_ptr<flutter_turbo_fetch::FlutterTurboFetchPlugin> cpp_plugin;
};

G_DEFINE_TYPE(FlutterTurboFetchPlugin, flutter_turbo_fetch_plugin, g_object_get_type())

static void flutter_turbo_fetch_plugin_dispose(GObject* object) {
  FlutterTurboFetchPlugin* self = FLUTTER_TURBO_FETCH_PLUGIN(object);
  self->cpp_plugin.reset();
  G_OBJECT_CLASS(flutter_turbo_fetch_plugin_parent_class)->dispose(object);
}

static void flutter_turbo_fetch_plugin_class_init(FlutterTurboFetchPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_turbo_fetch_plugin_dispose;
}

static void flutter_turbo_fetch_plugin_init(FlutterTurboFetchPlugin* self) {}

void flutter_turbo_fetch_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  FlutterTurboFetchPlugin* plugin = FLUTTER_TURBO_FETCH_PLUGIN(
      g_object_new(flutter_turbo_fetch_plugin_get_type(), nullptr));

  plugin->cpp_plugin = std::make_unique<flutter_turbo_fetch::FlutterTurboFetchPlugin>();

  // In standard Flutter Linux, BinaryMessenger is obtained from registrar
  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  (void)messenger;

  g_object_unref(plugin);
}
#endif  // TURBO_STANDALONE_TEST

namespace flutter_turbo_fetch {

void FlutterTurboFetchPlugin::RegisterWithMessenger(flutter::BinaryMessenger* messenger) {
  auto plugin = std::make_unique<FlutterTurboFetchPlugin>();
  TurboFetchHostApi::SetUp(messenger, plugin.get());
}

FlutterTurboFetchPlugin::FlutterTurboFetchPlugin()
    : owned_session_manager_(std::make_unique<TurboSessionManager>()),
      executor_(std::make_unique<TurboHttpExecutor>(*owned_session_manager_)) {}

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
  executor_->CancelRequest(request_id);
  return std::nullopt;
}

void FlutterTurboFetchPlugin::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  executor_->Execute(request, std::move(on_result));
}

}  // namespace flutter_turbo_fetch
