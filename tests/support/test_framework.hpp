#pragma once

// Minimal deterministic test harness. No timeouts are used anywhere: a test
// either completes or it is a defect to be diagnosed.

#include "apf/result.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace apftest {

struct Failure {
  std::string message;
};

struct TestCase {
  std::string name;
  std::string file;
  int line{0};
  std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, const char* file, int line, std::function<void()> fn);
};

inline std::string display(bool value) { return value ? "true" : "false"; }
inline std::string display(const std::string& value) { return value; }
inline std::string display(std::string_view value) { return std::string(value); }
inline std::string display(const char* value) { return value == nullptr ? "<null>" : value; }
inline std::string display(const apf::Error& error) { return apf::to_string(error); }
inline std::string display(apf::ErrorCode code) { return apf::to_string(code); }

template <class T>
std::string display(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(value);
  } else {
    (void)value;
    return "<value>";
  }
}

bool check(bool condition, const char* file, int line, const std::string& expression);

template <class T>
bool check_status(const apf::Result<T>& status, const char* file, int line,
                  const std::string& expression) {
  if (status.ok()) {
    return true;
  }
  std::fprintf(stderr, "%s(%d): expected success from %s but got %s\n", file, line,
               expression.c_str(), apf::to_string(status.error()).c_str());
  return false;
}

template <class A, class B>
bool check_eq(const A& lhs, const B& rhs, const char* file, int line, const char* lhs_text,
              const char* rhs_text) {
  if (lhs == rhs) {
    return true;
  }
  std::fprintf(stderr, "%s(%d): CHECK_EQ failed: %s == %s\n    left:  %s\n    right: %s\n",
               file, line, lhs_text, rhs_text, display(lhs).c_str(), display(rhs).c_str());
  return false;
}

int run_all(int argc, char** argv);

/// Deterministic xorshift generator. The seed is printed when a property test
/// fails so that any failure is exactly reproducible.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    std::uint64_t x = state_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state_ = x;
    return x;
  }

  std::uint32_t below(std::uint32_t bound) {
    return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
  }

  std::uint64_t seed() const { return state_; }

 private:
  std::uint64_t state_;
};

}  // namespace apftest

#define APF_TEST(name)                                                            \
  static void name();                                                             \
  static const apftest::Registrar apf_registrar_##name(#name, __FILE__, __LINE__, \
                                                       name);                     \
  static void name()

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!apftest::check((condition), __FILE__, __LINE__, #condition)) {            \
      /* keep going: a failed check does not abort the test */                     \
    }                                                                             \
  } while (false)

#define CHECK_OK(expression)                                                      \
  do {                                                                            \
    auto apf_status_ = (expression);                                 \
    (void)apftest::check_status(apf_status_, __FILE__, __LINE__, #expression);     \
  } while (false)

#define CHECK_ERR(expression, expected)                                           \
  do {                                                                            \
    auto apf_status_ = (expression);                                 \
    if (apf_status_.ok()) {                                                       \
      std::fprintf(stderr, "%s(%d): expected failure %s but the call succeeded\n", \
                   __FILE__, __LINE__, #expression);                              \
      apftest::check(false, __FILE__, __LINE__, #expression);                     \
    } else if (apf_status_.error().code != (expected)) {                          \
      std::fprintf(stderr, "%s(%d): %s failed with %s, expected %s\n", __FILE__,   \
                   __LINE__, #expression, apf::to_string(apf_status_.error()).c_str(), \
                   apf::to_string(expected));                                     \
      apftest::check(false, __FILE__, __LINE__, #expression);                     \
    }                                                                             \
  } while (false)

#define CHECK_EQ(lhs, rhs) apftest::check_eq((lhs), (rhs), __FILE__, __LINE__, #lhs, #rhs)

#define REQUIRE(condition)                                                        \
  do {                                                                            \
    if (!apftest::check((condition), __FILE__, __LINE__, #condition)) {            \
      throw apftest::Failure{#condition};                                         \
    }                                                                             \
  } while (false)
