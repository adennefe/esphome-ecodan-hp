#pragma once
#include <cmath>

namespace esphome { namespace optimizer {
// Observations must come from received ACTIVE_TIME packets, never an entity's
// synthetic startup OFF. Unknown -> ON is an observation, not a start edge.
class CompressorStateTracker {
 public:
  struct Observation {
    bool valid{false}, changed{false}, initial{true};
    bool previous{false}, running{false};
    bool started() const { return valid && changed && !initial && !previous && running; }
    bool stopped() const { return valid && changed && !initial && previous && !running; }
  };
  Observation observe(float value) {
    Observation out;
    out.previous = running_;
    out.initial = !known_;
    if (!std::isfinite(value) || (value != 0.0f && value != 1.0f)) {
      known_ = false;
      return out;
    }
    out.valid = true;
    out.running = value == 1.0f;
    out.changed = !known_ || out.running != running_;
    running_ = out.running;
    known_ = true;
    return out;
  }
  bool known() const { return known_; }
  bool running() const { return known_ && running_; }
 private:
  bool known_{false}, running_{false};
};
} }
