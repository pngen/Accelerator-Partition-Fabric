#include "apf/time.hpp"

namespace apf {

std::uint64_t system_now_ms() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

Clock::~Clock() = default;

std::uint64_t SystemClock::now_ms() const noexcept { return system_now_ms(); }

ManualClock::ManualClock(std::uint64_t start_ms) noexcept : now_(start_ms) {}

std::uint64_t ManualClock::now_ms() const noexcept { return now_.load(); }

void ManualClock::advance(std::uint64_t delta_ms) noexcept {
  now_.fetch_add(delta_ms);
}

void ManualClock::set(std::uint64_t now_ms) noexcept { now_.store(now_ms); }

ClockPtr make_system_clock() { return std::make_shared<SystemClock>(); }

ClockPtr make_manual_clock(std::uint64_t start_ms) {
  return std::make_shared<ManualClock>(start_ms);
}

}  // namespace apf
