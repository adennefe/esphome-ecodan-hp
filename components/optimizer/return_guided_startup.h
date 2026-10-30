#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome {
namespace optimizer {

// Independent of ESPHome so the lifecycle and minimum-temperature completion can be host-tested.
class ReturnGuidedStartup {
 public:
  enum class Phase { IDLE, ACTIVE, WAIT_FOR_AA };
  struct Result {
    bool apply{false};
    float target{NAN};
    float minimum_override{NAN};
    float return_target{NAN};
  };

  bool compressor_event(uint32_t now, bool running, bool previous, bool initial,
                        bool enabled, bool eligible) {
    if (!running) {
      cancel();
      claimed_cycle_ = false;
      return false;
    }
    if (previous || initial || !enabled || !eligible) return false;
    phase_ = Phase::ACTIVE;
    claimed_cycle_ = true;
    start_ms_ = now;
    last_target_ = NAN;
    completion_reason_ = "active";
    return true;
  }

  void cancel() { phase_ = Phase::IDLE; }
  Phase phase() const { return phase_; }
  bool owns_flow() const { return phase_ == Phase::ACTIVE; }
  bool claimed_cycle() const { return claimed_cycle_; }
  uint32_t elapsed_ms(uint32_t now) const { return now - start_ms_; }

  Result calculate(uint32_t now, bool enabled, bool eligible, float wp_return,
                   float offset, float duration_minutes, float normal_aa,
                   float zone_min, float zone_max) {
    Result result;
    if (!owns_flow()) return result;
    // A missing measurement must never be replaced with a fabricated return temp.
    if (!eligible || !std::isfinite(wp_return) || !std::isfinite(normal_aa)
        || !std::isfinite(zone_min) || !std::isfinite(zone_max)
        || wp_return < 0.0f || wp_return > 80.0f || zone_max < 20.0f || normal_aa < 20.0f) {
      cancel();
      return result;
    }
    if (!std::isfinite(offset)) offset = 1.0f;
    if (!enabled) {
      phase_ = Phase::WAIT_FOR_AA;
      return result;
    }
    if (!std::isfinite(duration_minutes)) duration_minutes = 15.0f;
    const uint32_t duration_ms = static_cast<uint32_t>(
        std::clamp(duration_minutes, 5.0f, 60.0f) * 60000.0f);
    offset = std::clamp(offset, 0.0f, 3.0f);
    result.return_target = wp_return + offset;
    const float finish_temperature = std::clamp(zone_min, 20.0f, zone_max);
    result.target = std::clamp(std::min({result.return_target, normal_aa, finish_temperature}), 20.0f, zone_max);
    if (elapsed_ms(now) >= duration_ms) {
      completion_reason_ = "maximum duration reached";
      phase_ = Phase::WAIT_FOR_AA;
      // Keep the last startup request until a regular AA cycle takes over.
      if (!std::isfinite(last_target_)) return Result{};
      result.target = std::clamp(last_target_, 20.0f, zone_max);
    } else if (std::round(result.target * 10.0f) / 10.0f >= finish_temperature) {
      completion_reason_ = "Z2 minimum reached";
      phase_ = Phase::WAIT_FOR_AA;
    }
    last_target_ = result.target;
    // The common writer rounds BEFORE clamping to its minimum. Keep this
    // temporary minimum on the same 0.1 C grid so clamping cannot undo rounding.
    result.minimum_override = std::round(std::max(20.0f, std::min(zone_min, result.target)) * 10.0f) / 10.0f;
    result.apply = true;
    return result;
  }

  void wait_for_aa() { phase_ = Phase::WAIT_FOR_AA; }
  const char *completion_reason() const { return completion_reason_; }
  bool waiting_for_aa() const { return phase_ == Phase::WAIT_FOR_AA; }
  void resume_normal_aa() { if (waiting_for_aa()) phase_ = Phase::IDLE; }

 private:
  Phase phase_{Phase::IDLE};
  bool claimed_cycle_{false};
  uint32_t start_ms_{0};
  float last_target_{NAN};
  const char *completion_reason_{"inactive"};
};

}  // namespace optimizer
}  // namespace esphome
