#ifndef FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_
#define FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_

#include <functional>
#include <memory>
#include <string>

#include "turbo_http_executor.h"

namespace flutter_turbo_fetch {

class FlutterTurboFetchPlugin {
 public:
  FlutterTurboFetchPlugin();
  explicit FlutterTurboFetchPlugin(std::unique_ptr<TurboHttpExecutor> executor);
  virtual ~FlutterTurboFetchPlugin();

  FlutterTurboFetchPlugin(const FlutterTurboFetchPlugin&) = delete;
  FlutterTurboFetchPlugin& operator=(const FlutterTurboFetchPlugin&) = delete;

  void Execute(const TurboHttpRequest& request,
               std::function<void(TurboHttpResponse)> on_result);

  void CancelRequest(const std::string& request_id);

  void Shutdown();

  TurboHttpExecutor* executor() const { return executor_.get(); }

 private:
  std::unique_ptr<TurboSessionManager> owned_session_manager_;
  std::unique_ptr<TurboHttpExecutor> executor_;
};

}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_PLUGIN_FLUTTER_TURBO_FETCH_PLUGIN_INTERNAL_H_

