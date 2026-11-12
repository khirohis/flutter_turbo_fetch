#include "flutter_turbo_fetch_plugin.h"

#ifndef TURBO_STANDALONE_TEST
#include "include/flutter_turbo_fetch/flutter_turbo_fetch_plugin.h"
#include <flutter_linux/flutter_linux.h>
#include "messages.g.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct _FlutterTurboFetchPlugin {
  GObject parent_instance;
  std::unique_ptr<flutter_turbo_fetch::FlutterTurboFetchPlugin> cpp_plugin;
};

G_DEFINE_TYPE(FlutterTurboFetchPlugin, flutter_turbo_fetch_plugin, g_object_get_type())

struct AsyncResponseData {
  FlutterTurboFetchTurboFetchHostApiResponseHandle* response_handle;
  flutter_turbo_fetch::TurboHttpResponse response;
};

static gboolean on_async_response_idle(gpointer user_data) {
  auto* data = static_cast<AsyncResponseData*>(user_data);

  int64_t* status_code_ptr = nullptr;
  int64_t status_code = 0;
  if (data->response.status_code() != nullptr) {
    status_code = *data->response.status_code();
    status_code_ptr = &status_code;
  }

  const gchar* status_message = data->response.status_message()
                                    ? data->response.status_message()->c_str()
                                    : nullptr;

  g_autoptr(FlValue) headers_val = fl_value_new_map();
  for (const auto& [k, vals] : data->response.headers()) {
    g_autoptr(FlValue) list_val = fl_value_new_list();
    for (const auto& v : vals) {
      fl_value_append_take(list_val, fl_value_new_string(v.c_str()));
    }
    fl_value_set_string_take(headers_val, k.c_str(), g_steal_pointer(&list_val));
  }

  const uint8_t* body_ptr =
      (data->response.body() != nullptr && !data->response.body()->empty())
          ? data->response.body()->data()
          : nullptr;
  size_t body_len =
      data->response.body() != nullptr ? data->response.body()->size() : 0;

  const gchar* error_code = data->response.error_code()
                                ? data->response.error_code()->c_str()
                                : nullptr;
  const gchar* error_message = data->response.error_message()
                                   ? data->response.error_message()->c_str()
                                   : nullptr;

  FlutterTurboFetchTurboHttpResponse* g_response =
      flutter_turbo_fetch_turbo_http_response_new(
          status_code_ptr, status_message, headers_val, body_ptr, body_len,
          error_code, error_message);

  flutter_turbo_fetch_turbo_fetch_host_api_respond_execute_request(
      data->response_handle, g_response);

  g_object_unref(data->response_handle);
  delete data;
  return G_SOURCE_REMOVE;
}

static void on_execute_request(
    FlutterTurboFetchTurboHttpRequest* request,
    FlutterTurboFetchTurboFetchHostApiResponseHandle* response_handle,
    gpointer user_data) {
  auto* self = FLUTTER_TURBO_FETCH_PLUGIN(user_data);

  const gchar* req_id =
      flutter_turbo_fetch_turbo_http_request_get_request_id(request);
  const gchar* url = flutter_turbo_fetch_turbo_http_request_get_url(request);
  const gchar* method =
      flutter_turbo_fetch_turbo_http_request_get_method(request);

  FlValue* headers =
      flutter_turbo_fetch_turbo_http_request_get_headers(request);
  std::unordered_map<std::string, std::string> cpp_headers;
  if (headers && fl_value_get_type(headers) == FL_VALUE_TYPE_MAP) {
    size_t len = fl_value_get_length(headers);
    for (size_t i = 0; i < len; ++i) {
      FlValue* k = fl_value_get_map_key(headers, i);
      FlValue* v = fl_value_get_map_value(headers, i);
      if (k && fl_value_get_type(k) == FL_VALUE_TYPE_STRING &&
          v && fl_value_get_type(v) == FL_VALUE_TYPE_STRING) {
        cpp_headers[fl_value_get_string(k)] = fl_value_get_string(v);
      }
    }
  }

  size_t body_len = 0;
  const uint8_t* body =
      flutter_turbo_fetch_turbo_http_request_get_body(request, &body_len);
  std::vector<uint8_t> cpp_body;
  const std::vector<uint8_t>* cpp_body_ptr = nullptr;
  if (body && body_len > 0) {
    cpp_body.assign(body, body + body_len);
    cpp_body_ptr = &cpp_body;
  }

  int64_t* conn_timeout =
      flutter_turbo_fetch_turbo_http_request_get_connect_timeout_ms(request);
  int64_t* recv_timeout =
      flutter_turbo_fetch_turbo_http_request_get_receive_timeout_ms(request);
  int64_t* send_timeout =
      flutter_turbo_fetch_turbo_http_request_get_send_timeout_ms(request);
  gboolean follow_redirects =
      flutter_turbo_fetch_turbo_http_request_get_follow_redirects(request);
  int64_t max_conns =
      flutter_turbo_fetch_turbo_http_request_get_max_requests_per_host(request);
  gboolean allow_bad_certs =
      flutter_turbo_fetch_turbo_http_request_get_allow_bad_certificates(request);
  int64_t* max_bytes =
      flutter_turbo_fetch_turbo_http_request_get_max_response_bytes(request);

  flutter_turbo_fetch::TurboHttpRequest cpp_req(
      req_id ? req_id : "", url ? url : "", method ? method : "GET",
      std::move(cpp_headers), cpp_body_ptr, conn_timeout, recv_timeout,
      send_timeout, follow_redirects, max_conns, allow_bad_certs, max_bytes);

  g_object_ref(response_handle);
  self->cpp_plugin->Execute(
      cpp_req,
      [response_handle](flutter_turbo_fetch::TurboHttpResponse response) {
        auto* data = new AsyncResponseData{response_handle, std::move(response)};
        g_idle_add(on_async_response_idle, data);
      });
}

static FlutterTurboFetchTurboFetchHostApiCancelRequestResponse*
on_cancel_request(const gchar* request_id, gpointer user_data) {
  auto* self = FLUTTER_TURBO_FETCH_PLUGIN(user_data);
  if (request_id) {
    self->cpp_plugin->CancelRequest(request_id);
  }
  return flutter_turbo_fetch_turbo_fetch_host_api_cancel_request_response_new();
}

static const FlutterTurboFetchTurboFetchHostApiVTable g_host_api_vtable = {
    .execute_request = on_execute_request,
    .cancel_request = on_cancel_request,
};

static void flutter_turbo_fetch_plugin_dispose(GObject* object) {
  FlutterTurboFetchPlugin* self = FLUTTER_TURBO_FETCH_PLUGIN(object);
  if (self->cpp_plugin) {
    self->cpp_plugin->Shutdown();
    self->cpp_plugin.reset();
  }
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

  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  flutter_turbo_fetch_turbo_fetch_host_api_set_method_handlers(
      messenger, nullptr, &g_host_api_vtable, g_object_ref(plugin),
      g_object_unref);

  g_object_unref(plugin);
}
#endif  // TURBO_STANDALONE_TEST

namespace flutter_turbo_fetch {

FlutterTurboFetchPlugin::FlutterTurboFetchPlugin()
    : owned_session_manager_(std::make_unique<TurboSessionManager>()),
      executor_(std::make_unique<TurboHttpExecutor>(*owned_session_manager_)) {}

FlutterTurboFetchPlugin::FlutterTurboFetchPlugin(
    std::unique_ptr<TurboHttpExecutor> executor)
    : executor_(std::move(executor)) {}

FlutterTurboFetchPlugin::~FlutterTurboFetchPlugin() {
  Shutdown();
}

void FlutterTurboFetchPlugin::Shutdown() {
  if (executor_) {
    executor_->Shutdown();
  }
}

void FlutterTurboFetchPlugin::Execute(
    const TurboHttpRequest& request,
    std::function<void(TurboHttpResponse)> on_result) {
  executor_->Execute(request, std::move(on_result));
}

void FlutterTurboFetchPlugin::CancelRequest(const std::string& request_id) {
  executor_->CancelRequest(request_id);
}

}  // namespace flutter_turbo_fetch
