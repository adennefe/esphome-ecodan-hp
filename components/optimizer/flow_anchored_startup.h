#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome { namespace optimizer {

// One owner for both time-based and optional THW10-guided startup. No frequency control.
class FlowAnchoredStartup {
 public:
  enum class Phase { WAITING, BUFFER_WARMUP, HANDOVER, NORMAL_AA };
  struct Result { bool apply{false}; float target{NAN}, minimum{NAN}, cap{NAN}; };
  struct Buffer {
    bool enabled{false}, valid{false};
    float temperature{NAN}, z2_feed{NAN}, margin{0.5f}, ready_minutes{5};
  };
  static bool valid_thw10(bool configured, float temperature) {
    return configured && std::isfinite(temperature) && temperature >= 0 && temperature <= 80;
  }
  bool compressor_event(uint32_t now, bool running, bool previous, bool initial,
                        bool enabled, bool eligible, float start_return, float delta) {
    if (!running) { cancel("compressor stopped"); phase_=Phase::WAITING; claimed_=false; return false; }
    if (initial) { if (!active()) phase_=Phase::NORMAL_AA; return false; }
    if (previous || claimed_) return false;
    if (!enabled) { if (!active()) phase_=Phase::NORMAL_AA; return false; }
    if (phase_!=Phase::WAITING) return false;
    claimed_=true; phase_=Phase::NORMAL_AA;
    if (!eligible || !std::isfinite(start_return) || start_return<0 || start_return>80 || !std::isfinite(delta)) {
      cancel("start not eligible"); return false;
    }
    phase_=Phase::BUFFER_WARMUP; start_ms_=now; initial_return_=start_return;
    base_=start_return+std::clamp(delta,0.0f,5.0f); cap_=base_;
    last_requested_=normal_=threshold_=NAN; ready_completed_ms_=0;
    ready_.reset(); room_need_.reset(); guard_.reset();
    reason_="genuine compressor start";
    return true;
  }
  void cancel(const char *reason) { if (phase_!=Phase::WAITING) phase_=Phase::NORMAL_AA; reason_=reason; }
  bool active() const { return phase_==Phase::BUFFER_WARMUP || phase_==Phase::HANDOVER; }
  Phase phase() const { return phase_; }
  const char *phase_name() const { return phase_name(phase_); }
  static const char *phase_name(Phase phase) {
    switch(phase) {
      case Phase::WAITING: return "WAITING";
      case Phase::BUFFER_WARMUP: return "BUFFER_WARMUP";
      case Phase::HANDOVER: return "HANDOVER";
      default: return "NORMAL_AA";
    }
  }
  bool claimed_cycle() const { return claimed_; }
  float initial_return() const { return initial_return_; }
  float base() const { return base_; }
  float cap() const { return cap_; }
  float buffer_threshold() const { return threshold_; }
  float last_requested() const { return last_requested_; }
  const char *reason() const { return reason_; }
  uint32_t elapsed_ms(uint32_t now) const { return now-start_ms_; }
  uint32_t ready_ms(uint32_t now) const { return phase_==Phase::BUFFER_WARMUP ? ready_.elapsed(now) : ready_completed_ms_; }
  uint32_t room_need_ms(uint32_t now) const { return room_need_.elapsed(now); }
  uint32_t guard_ms(uint32_t now) const { return guard_.elapsed(now); }

  void observe_room(uint32_t now,float room,float target) {
    if (!active()) return;
    if (!std::isfinite(room)||!std::isfinite(target)) { cancel("invalid room temperature"); return; }
    const float deficit=target-room;
    if (deficit>=0.5f-kTemperatureEpsilon) { cancel("urgent room deficit at least 0.5 K"); return; }
    room_need_.update(now,deficit>=0.2f-kTemperatureEpsilon);
    if (room_need_.elapsed(now)>=600000UL) cancel("room deficit at least 0.2 K for ten minutes");
  }

  // These timestamps measure fulfilled CONDITIONS. No THW10 reception time or value-change test exists.
  void observe_buffer(uint32_t now,const Buffer &buffer,float zone_min,float zone_max,bool allow_handover=true) {
    if (phase_!=Phase::BUFFER_WARMUP) return;
    if (!std::isfinite(normal_)||!std::isfinite(zone_min)||!std::isfinite(zone_max)||zone_min>zone_max) {
      ready_.reset(); threshold_=NAN; return;
    }
    const float margin=std::isfinite(buffer.margin) ? std::clamp(buffer.margin,0.0f,3.0f) : 0.5f;
    threshold_=std::max(zone_min,std::clamp(normal_,zone_min,zone_max)-margin);
    // The same supply threshold must be reached at Z2: a hot tank point alone is insufficient.
    const bool condition=buffer.enabled && buffer.valid && valid_thw10(true,buffer.temperature)
        && std::isfinite(buffer.z2_feed) && buffer.z2_feed>=threshold_-kTemperatureEpsilon
        && buffer.temperature>=threshold_-kTemperatureEpsilon;
    ready_.update(now,condition);
    const float duration=std::isfinite(buffer.ready_minutes) ? std::clamp(buffer.ready_minutes,1.0f,30.0f) : 5.0f;
    if (allow_handover && condition && ready_.elapsed(now)>=static_cast<uint32_t>(duration*60000)
        && std::isfinite(last_requested_)) {
      ready_completed_ms_=ready_.elapsed(now);
      phase_=Phase::HANDOVER; handover_step_ms_=now;
      reason_="THW10 and Z2 supply ready for configured condition duration";
    }
  }

  Result calculate(uint32_t now,bool enabled,bool eligible,float normal,float room,float room_target,
                   float step,float interval_minutes,float duration_minutes,float zone_min,float zone_max,
                   const Buffer &buffer) {
    if (!active()) return {};
    if (!enabled || !eligible) { cancel(!enabled ? "disabled by user" : "eligibility lost"); return {}; }
    if (!std::isfinite(normal)||!std::isfinite(zone_min)||!std::isfinite(zone_max)||zone_min<20||zone_max<20
        ||zone_min>zone_max||!std::isfinite(step)||!std::isfinite(interval_minutes)||!std::isfinite(duration_minutes)) {
      cancel("invalid AA target, configuration or limits"); return {};
    }
    if (elapsed_ms(now)>=static_cast<uint32_t>(std::clamp(duration_minutes,5.0f,60.0f)*60000)) {
      cancel("maximum total startup and handover duration reached"); return {};
    }
    observe_room(now,room,room_target);
    if (!active()) return {};
    normal_=normal;
    observe_buffer(now,buffer,zone_min,zone_max);
    float requested;
    if (phase_==Phase::BUFFER_WARMUP) {
      const auto interval=static_cast<uint32_t>(std::clamp(interval_minutes,1.0f,30.0f)*60000);
      cap_=base_+(elapsed_ms(now)/interval)*std::clamp(step,0.1f,2.0f);
      requested=std::clamp(std::min(normal,cap_),20.0f,zone_max);
    } else {
      const float target=std::clamp(normal,zone_min,zone_max);
      // Start from the last request actually queued, or confirmed unchanged, by the existing writer.
      if (!std::isfinite(last_requested_)) { cancel("handover has no confirmed request baseline"); return {}; }
      const float increment=(now-handover_step_ms_>=180000UL) ? 0.5f : 0.0f;
      cap_=std::min(zone_max,last_requested_+increment);
      requested=std::clamp(std::min(target,cap_),20.0f,zone_max);
      if (requested>=target-kTemperatureEpsilon) cancel("handover reached normal AA including configured minimum");
    }
    const float minimum=std::round(std::max(20.0f,std::min(zone_min,requested))*10)/10;
    return {true,requested,minimum,cap_};
  }
  // Compatibility for time-only callers and regression fixtures.
  Result calculate(uint32_t now,bool enabled,bool eligible,float normal,float room,float room_target,
                   float step,float interval_minutes,float duration_minutes,float zone_min,float zone_max) {
    return calculate(now,enabled,eligible,normal,room,room_target,step,interval_minutes,duration_minutes,zone_min,zone_max,Buffer{});
  }
  bool guard_result(uint32_t now,bool stop_guard,float before,float after) {
    if (!active()) return false;
    guard_.update(now,stop_guard && std::isfinite(before) && std::isfinite(after) && after-before>=0.5f-kTemperatureEpsilon);
    if (guard_.elapsed(now)<300000UL) return false;
    cancel("compressor-stop guard override at least 0.5 K for five minutes"); return true;
  }
  void record_request(uint32_t now,float request) {
    if (!active()||!std::isfinite(request)) return;
    if (phase_==Phase::HANDOVER && (!std::isfinite(last_requested_)||request>last_requested_+0.05f)) handover_step_ms_=now;
    last_requested_=request;
  }
 private:
  struct Condition {
    bool on{false}; uint32_t since{0};
    void reset() { on=false; }
    void update(uint32_t now,bool fulfilled) { if (!fulfilled) reset(); else if (!on) { on=true; since=now; } }
    uint32_t elapsed(uint32_t now) const { return on ? now-since : 0; }
  } ready_,room_need_,guard_;
  static constexpr float kTemperatureEpsilon=0.0001f;
  Phase phase_{Phase::WAITING}; bool claimed_{false};
  uint32_t start_ms_{0},handover_step_ms_{0},ready_completed_ms_{0};
  float initial_return_{NAN},base_{NAN},cap_{NAN},normal_{NAN},threshold_{NAN},last_requested_{NAN};
  const char *reason_{"waiting for genuine compressor start"};
};
} }  // namespace esphome::optimizer
