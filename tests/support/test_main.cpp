#include "support/test_framework.hpp"

#include <cstring>
#include <string>

namespace apftest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, const char* file, int line, std::function<void()> fn) {
  TestCase test;
  test.name = name;
  test.file = file;
  test.line = line;
  test.fn = std::move(fn);
  registry().push_back(std::move(test));
}

bool check(bool condition, const char* file, int line, const std::string& expression) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", file, line, expression.c_str());
  return false;
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list = false;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--list") == 0) {
      list = true;
    } else if (std::strncmp(argv[index], "--filter=", 9) == 0) {
      filter = argv[index] + 9;
    }
  }
  if (list) {
    for (const TestCase& test : registry()) {
      std::printf("%s\n", test.name.c_str());
    }
    return 0;
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    std::printf("[ RUN  ] %s\n", test.name.c_str());
    std::fflush(stdout);
    try {
      test.fn();
      std::printf("[  OK  ] %s\n", test.name.c_str());
      ++passed;
    } catch (const Failure& failure) {
      std::printf("[ FAIL ] %s (requirement failed: %s)\n", test.name.c_str(),
                  failure.message.c_str());
      failures.push_back(test.name);
      ++failed;
    } catch (const std::exception& error) {
      std::printf("[ FAIL ] %s (exception: %s)\n", test.name.c_str(), error.what());
      failures.push_back(test.name);
      ++failed;
    } catch (...) {
      std::printf("[ FAIL ] %s (unknown exception)\n", test.name.c_str());
      failures.push_back(test.name);
      ++failed;
    }
    std::fflush(stdout);
  }
  std::printf("\n%zu passed, %zu failed\n", passed, failed);
  if (!failures.empty()) {
    std::printf("failing tests:\n");
    for (const std::string& name : failures) {
      std::printf("  %s\n", name.c_str());
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace apftest

int main(int argc, char** argv) { return apftest::run_all(argc, argv); }
