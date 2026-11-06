#include "flutter_turbo_fetch_plugin.h"

#include <memory>
#include <optional>
#include <string>

#include "test/test_assertions.h"
#include "turbo_http_executor.h"

namespace flutter_turbo_fetch {
namespace test {

namespace {

class FakeTurboHttpExecutor : public TurboHttpExecutor {
 public:
  void Execute(const TurboHttpRequest& request,
               std::function<void(TurboHttpResponse)> on_result) override {
    captured_request_id = request.request_id();
    captured_on_result = std::move(on_result);
  }

  void Cancel(const std::string& request_id) override {
    canceled_request_id = request_id;
  }

  std::string captured_request_id;
  std::function<void(TurboHttpResponse)> captured_on_result;
  std::string canceled_request_id;
};

}  // namespace

TURBO_TEST(FlutterTurboFetchPlugin_ExecuteAndCancelDelegateToExecutor) {
  auto fake_executor = std::make_unique<FakeTurboHttpExecutor>();
  FakeTurboHttpExecutor* raw_fake = fake_executor.get();

  FlutterTurboFetchPlugin plugin(std::move(fake_executor));

  const int64_t timeout_ms = 5000;
  TurboHttpRequest request("req-cancel-test", "https://example.com/data", "GET",
                           flutter::EncodableMap{}, nullptr, &timeout_ms,
                           &timeout_ms, &timeout_ms, true, 32, false, nullptr);

  std::optional<TurboHttpResponse> received_response;
  plugin.ExecuteRequest(request, [&](ErrorOr<TurboHttpResponse> reply) {
    ASSERT_FALSE(reply.has_error());
    received_response = reply.value();
  });

  ASSERT_EQ(std::string("req-cancel-test"), raw_fake->captured_request_id);
  ASSERT_FALSE(received_response.has_value());

  std::optional<FlutterError> cancel_error =
      plugin.CancelRequest("req-cancel-test");
  ASSERT_FALSE(cancel_error.has_value());
  ASSERT_EQ(std::string("req-cancel-test"), raw_fake->canceled_request_id);

  TurboHttpResponse expected_res;
  expected_res.set_error_code("cancel");
  expected_res.set_error_message("Canceled");
  raw_fake->captured_on_result(expected_res);

  ASSERT_TRUE(received_response.has_value());
  ASSERT_NE(nullptr, received_response->error_code());
  ASSERT_EQ(std::string("cancel"), *received_response->error_code());
}

}  // namespace test
}  // namespace flutter_turbo_fetch

int main() { return ::flutter_turbo_fetch::test::RunAllTests(); }

