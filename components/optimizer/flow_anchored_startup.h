#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome { namespace optimizer {

// Only a confirmed OFF -> ON event can claim a cycle. No restored runtime state.
class FlowAnchoredStartup {
 public:
  struct Result { bool apply{false}; float target{NAN}, minimum{NAN}, cap{NAN}; };
  bool compressor_event(uint32_t now, bool running, bool previous, bool initial,
                        bool enabled, bool eligible, float start_return, float delta) {
    if (!running) { cancel("compressor stopped"); claimed_ = false; return false; }
    if (previous || initial || !enabled) return false;
    claimed_ = true;  // Selected alternative also suppresses old ramps if ineligible.
    if (!eligible || !std::isfinite(start_return) || start_return < 0 || start_return > 80
        || !std::isfinite(delta)) { cancel("start not eligible"); return false; }
    active_ = true;
    hold_ = low_tracking_ = false;
    start_ms_ = now;
    initial_return_ = start_return;
    base_ = start_return + std::clamp(delta, 0.0f, 5.0f);
    cap_ = base_;
    reason_ = "genuine compressor start";
    guard_count_ = 0;
    guard_seen_ = false;
    return true;
  }
  void cancel(const char *reason) { active_ = hold_ = low_tracking_ = false; reason_ = reason; }
  bool active() const { return active_; }
  bool claimed_cycle() const { return claimed_; }
  bool holding() const { return hold_; }
  float initial_return() const { return initial_return_; }
  float base() const { return base_; }
  float cap() const { return cap_; }
  const char *reason() const { return reason_; }
  uint32_t elapsed_ms(uint32_t now) const { return now - start_ms_; }

  // Called for every actual frequency observation, not for repeated AA evaluations.
  void observe_frequency(uint32_t now, float hz) {
    if (!active_) return;
    const bool in_band = std::isfinite(hz) && hz >= 10 && hz <= 30;
    if (!in_band || (low_tracking_ && now - frequency_ms_ > 120000UL)) {
      low_tracking_ = false;
      if (hold_) { hold_ = false; reason_ = "HOLD released: frequency outside 10..30 Hz or observation gap"; }
    }
    if (in_band && !low_tracking_) { low_tracking_ = true; low_since_ = now; }
    frequency_ms_ = now;
  }

  // Room demand must stay near target for the whole qualification window too.
  void observe_room(uint32_t now, float room, float target) {
    if (!active_) return;
    if (!std::isfinite(room) || !std::isfinite(target) || room < target - 0.1f - 0.0001f) {
      low_tracking_ = false;
      if (hold_) { hold_ = false; reason_ = "HOLD released: room heat demand increased"; }
    }
    if (std::isfinite(room) && std::isfinite(target) && room <= target - 0.2f + 0.0001f)
      cancel("room deficit at least 0.2 K");
    (void) now;
  }

  Result calculate(uint32_t now, bool enabled, bool eligible, float normal,
                   float room, float room_target, float step, float interval_minutes,
                   float duration_minutes, float zone_min, float zone_max) {
    if (!active_) return {};
    if (!enabled || !eligible) { cancel(!enabled ? "disabled by user" : "eligibility lost"); return {}; }
    if (!std::isfinite(normal) || !std::isfinite(room) || !std::isfinite(room_target)
        || !std::isfinite(zone_min) || !std::isfinite(zone_max) || zone_max < 20
        || zone_min > zone_max || !std::isfinite(step) || !std::isfinite(interval_minutes)
        || !std::isfinite(duration_minutes)) { cancel("invalid AA target, configuration or limits"); return {}; }
    if (elapsed_ms(now) >= static_cast<uint32_t>(std::clamp(duration_minutes, 5.0f, 60.0f) * 60000)) {
      cancel("maximum startup duration reached"); return {};
    }
    observe_room(now, room, room_target);
    if (!active_) return {};
    if (low_tracking_ && now - frequency_ms_ > 120000UL) {
      low_tracking_ = false;
      if (hold_) { hold_ = false; reason_ = "HOLD released: stale frequency"; }
    }
    const auto interval = static_cast<uint32_t>(std::clamp(interval_minutes, 1.0f, 30.0f) * 60000);
    const float timed_cap = base_ + (elapsed_ms(now) / interval) * std::clamp(step, 0.1f, 2.0f);
    // Freeze the cap reached at qualification, not the previous evaluation's cap.
    if (!hold_ && low_tracking_ && now - low_since_ >= 300000UL) {
      cap_ = timed_cap;
      hold_ = true; reason_ = "HOLD: continuous low modulation and room near target";
    }
    if (!hold_) cap_ = timed_cap;
    const float requested = std::clamp(std::min(normal, cap_), 20.0f, zone_max);
    const float minimum = std::round(std::max(20.0f, std::min(zone_min, requested)) * 10) / 10;
    return {true, requested, minimum, cap_};
  }

  // Two consecutive stop-guard interventions, at least 30 s apart, release the cap.
  bool guard_result(uint32_t now, bool stop_guard) {
    if (!active_) return false;
    if (!stop_guard) { guard_count_ = 0; guard_seen_ = false; return false; }
    if (!guard_seen_ || now - guard_ms_ >= 30000UL) {
      guard_seen_ = true; guard_ms_ = now; ++guard_count_;
    }
    if (guard_count_ < 2) return false;
    cancel("repeated compressor-stop guard interventions");
    return true;
  }

 private:
  bool active_{false}, claimed_{false}, hold_{false}, low_tracking_{false}, guard_seen_{false};
  uint32_t start_ms_{0}, low_since_{0}, frequency_ms_{0}, guard_ms_{0};
  unsigned guard_count_{0};
  float initial_return_{NAN}, base_{NAN}, cap_{NAN};
  const char *reason_{"inactive"};
};
} }  // namespace esphome::optimizer
