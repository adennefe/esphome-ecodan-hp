#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace esphome { namespace optimizer {
struct GuardTelemetry {
  enum Field { FREQUENCY, FLOW_RATE, HP_FEED, HP_RETURN, Z2_FEED, Z2_RETURN, COUNT };
  std::array<float, COUNT> values{{NAN, NAN, NAN, NAN, NAN, NAN}};
  std::array<uint32_t, COUNT> received{};
  std::array<bool, COUNT> observed{};
  void observe(Field field, uint32_t now, float value) {
    values[field] = value;
    received[field] = now;
    observed[field] = std::isfinite(value);
  }
  bool fresh(uint32_t now) const {
    for (unsigned i=0; i<COUNT; ++i)
      if (!observed[i] || now-received[i]>120000UL) return false;
    return true;
  }
};

// Part of the existing guard write path. Never relaxes its current floor.
// It can release a held uplift, not command a lower flow than the old AA guard.
class GuardRecovery {
 public:
  struct Result {
    float target;
    float uplift;
    bool permitted{false}, step{false};
    const char *reason{"legacy fallback"};
  };
  void reset() { tracked_=window_=stepped_=false; held_=NAN; }
  Result update(uint32_t now, bool eligible, float normal, float required,
                float legacy, float live, const GuardTelemetry &samples) {
    Result out{legacy, std::max(0.0f, legacy-normal)};
    if (!eligible || !samples.fresh(now) || !std::isfinite(normal)
        || !std::isfinite(required) || !std::isfinite(legacy) || !std::isfinite(live)) {
      reset(); return out;
    }
    const auto &v=samples.values;
    if (v[GuardTelemetry::FREQUENCY]<16 || v[GuardTelemetry::FREQUENCY]>26
        || v[GuardTelemetry::FLOW_RATE]<5) {
      reset(); out.reason="frequency or flow outside observation window"; return out;
    }
    for (unsigned i=GuardTelemetry::HP_FEED; i<GuardTelemetry::COUNT; ++i) {
      if (v[i]<0 || v[i]>80) { reset(); out.reason="invalid water temperature"; return out; }
    }
    if (v[GuardTelemetry::HP_FEED]+0.2f<v[GuardTelemetry::HP_RETURN]
        || v[GuardTelemetry::Z2_FEED]+0.2f<v[GuardTelemetry::Z2_RETURN]) {
      reset(); out.reason="inconsistent heating feed/return measurements"; return out;
    }
    // Never infer a confirmed request from a still-pending or external command.
    const bool confirmed=tracked_ && std::fabs(live-held_)<0.11f;
    if (!window_ || now-last_observation_>90000UL) start_window_(now,samples);
    last_observation_=now;
    bool stable=true;
    for (unsigned i=0;i<GuardTelemetry::COUNT;++i) {
      low_[i]=std::min(low_[i],v[i]); high_[i]=std::max(high_[i],v[i]);
      const float span=i==GuardTelemetry::FREQUENCY ? 4.0f : i==GuardTelemetry::FLOW_RATE ? 2.0f : 0.5f;
      if (high_[i]-low_[i]>span+0.001f) stable=false;
    }
    // A rising temperature is not evidence that an uplift can be removed.
    if (v[GuardTelemetry::HP_FEED]>first_[GuardTelemetry::HP_FEED]+0.2f
        || v[GuardTelemetry::Z2_FEED]>first_[GuardTelemetry::Z2_FEED]+0.2f) stable=false;
    if (!stable || (tracked_ && legacy>held_+0.05f)) {
      start_window_(now,samples); stepped_=false;
      out.reason="new guard intervention or unstable measurements";
    } else if (now-stable_since_>=600000UL && confirmed) {
      out.permitted=true;
      if (required>=live-0.05f) {
        out.reason="existing guard floor prevents further reduction";
      } else {
        // One step per ten minutes, independent of callback count. The current
        // legacy guard floor, normal AA target and later max clamp always win.
        const bool due=!stepped_ || now-last_step_>=600000UL;
        out.target=std::max(required, due ? live-0.5f : live);
        out.target=std::max(normal,out.target);
        if (out.target<live-0.05f && due) {
          last_step_=now; stepped_=true; out.step=true;
          out.reason="stable recovery step within existing guard floor";
        } else out.reason="waiting for next recovery step";
      }
    } else out.reason=confirmed ? "collecting ten minutes of stable measurements" : "waiting for confirmed target";
    tracked_=out.target>normal+0.05f;
    held_=out.target;
    out.uplift=std::max(0.0f,out.target-normal);
    return out;
  }
 private:
  void start_window_(uint32_t now,const GuardTelemetry &s) {
    window_=true; stable_since_=now; first_=low_=high_=s.values;
  }
  bool tracked_{false},window_{false},stepped_{false};
  float held_{NAN};
  uint32_t stable_since_{0},last_observation_{0},last_step_{0};
  std::array<float,GuardTelemetry::COUNT> first_{},low_{},high_{};
};
} }
