#ifndef FLUTTER_TURBO_FETCH_LINUX_TEST_TEST_ASSERTIONS_H_
#define FLUTTER_TURBO_FETCH_LINUX_TEST_TEST_ASSERTIONS_H_

#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace flutter_turbo_fetch {
namespace test {

struct TestCase {
  std::string name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& GetRegistry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct TestRegistrar {
  TestRegistrar(const char* name, std::function<void()> fn) {
    GetRegistry().push_back({name, std::move(fn)});
  }
};

#define TURBO_TEST(test_name)                                                  \
  static void test_name();                                                     \
  static ::flutter_turbo_fetch::test::TestRegistrar registrar_##test_name(     \
      #test_name, test_name);                                                  \
  static void test_name()

#define ASSERT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::ostringstream oss;                                                  \
      oss << __FILE__ << ":" << __LINE__ << ": ASSERT_TRUE(" << #cond          \
          << ") failed.";                                                      \
      throw std::runtime_error(oss.str());                                     \
    }                                                                          \
  } while (false)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(expected, actual)                                            \
  do {                                                                         \
    const auto& exp_val = (expected);                                          \
    const auto& act_val = (actual);                                            \
    if (!(exp_val == act_val)) {                                               \
      std::ostringstream oss;                                                  \
      oss << __FILE__ << ":" << __LINE__ << ": ASSERT_EQ(" << #expected        \
          << ", " << #actual << ") failed.";                                   \
      throw std::runtime_error(oss.str());                                     \
    }                                                                          \
  } while (false)

#define ASSERT_NE(val1, val2)                                                  \
  do {                                                                         \
    if ((val1) == (val2)) {                                                    \
      std::ostringstream oss;                                                  \
      oss << __FILE__ << ":" << __LINE__ << ": ASSERT_NE(" << #val1 << ", "    \
          << #val2 << ") failed.";                                             \
      throw std::runtime_error(oss.str());                                     \
    }                                                                          \
  } while (false)

inline int RunAllTests() {
  int passed = 0;
  int failed = 0;
  for (const auto& tc : GetRegistry()) {
    try {
      tc.fn();
      std::cout << "[       OK ] " << tc.name << std::endl;
      ++passed;
    } catch (const std::exception& e) {
      std::cerr << "[  FAILED  ] " << tc.name << "\n  " << e.what()
                << std::endl;
      ++failed;
    } catch (...) {
      std::cerr << "[  FAILED  ] " << tc.name << " (unknown exception)"
                << std::endl;
      ++failed;
    }
  }
  std::cout << passed << " passed, " << failed << " failed." << std::endl;
  return failed == 0 ? 0 : 1;
}

}  // namespace test
}  // namespace flutter_turbo_fetch

#endif  // FLUTTER_TURBO_FETCH_LINUX_TEST_TEST_ASSERTIONS_H_

