#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace apf {

/// Milliseconds since the Unix epoch, used only for evidence freshness and
/// human-facing timestamps. Freshness decisions are always explicit and
/// injectable so that determinism can be proven.
std::uint64_t system_now_ms() noexcept;

class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  virtual std::uint64_t now_ms() const noexcept = 0;
};

using ClockPtr = std::shared_ptr<Clock>;

/// Real wall clock.
class SystemClock final : public Clock {
 public:
  std::uint64_t now_ms() const noexcept override;
};

/// Deterministic clock for tests: time only moves when told to.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::uint64_t start_ms = 1'000'000) noexcept;
  std::uint64_t now_ms() const noexcept override;
  void advance(std::uint64_t delta_ms) noexcept;
  void set(std::uint64_t now_ms) noexcept;

 private:
  std::atomic<std::uint64_t> now_;
};

ClockPtr make_system_clock();
ClockPtr make_manual_clock(std::uint64_t start_ms = 1'000'000);

}  // namespace apf
