#include "components/optimizer/return_guided_startup.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using esphome::optimizer::ReturnGuidedStartup;
using Phase = ReturnGuidedStartup::Phase;
static int checks = 0;
static void check(bool ok, const char *description) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
static ReturnGuidedStartup started(uint32_t now = 0) {
  ReturnGuidedStartup s;
  check(s.compressor_event(now, true, false, false, true, true), "genuine eligible start");
  return s;
}
static ReturnGuidedStartup::Result tick(ReturnGuidedStartup &s, uint32_t now, float ret = 20.5f,
                                        float raw = 26.0f, bool enabled = true, bool eligible = true) {
  return s.calculate(now, enabled, eligible, ret, 1.0f, 15.0f, raw, 24.0f, 35.0f);
}

int main() {
  {
    ReturnGuidedStartup s;
    check(!s.compressor_event(0, true, false, false, false, true), "OFF: no arm");
    check(!tick(s, 1000).apply && !s.claimed_cycle(), "OFF: ordinary path and legacy ramps retained");
    check(!s.compressor_event(0, true, false, true, true, true), "ESP boot while compressor running does not arm");
    check(!s.compressor_event(1000, true, true, false, true, true), "repeated ON cannot rearm");
    s.compressor_event(2000, false, true, false, true, true);
    check(s.compressor_event(3000, true, false, false, true, true), "next genuine start after boot can arm");
  }
  {
    auto s = started();
    auto r = tick(s, 0);
    check(r.apply && near(r.target, 21.5f) && near(r.minimum_override, 21.5f), "20.5 return -> 21.5, temporary min overrides 24");
    check(near(tick(s, 30000, 22.0f, 22.5f).target, 22.5f), "normal AA before minimum caps startup");
    check(near(tick(s, 60000, 23.0f).target, 24.0f), "startup caps at configured minimum");
    check(s.waiting_for_aa() && !s.owns_flow(), "minimum reached: wait for regular AA");
    check(!tick(s, 90000).apply, "no extra startup writes after minimum");
    check(s.claimed_cycle(), "legacy ramps suppressed for claimed cycle");
    s.compressor_event(61000, false, true, false, true, true);
    check(!s.owns_flow() && !s.claimed_cycle(), "stop resets flow and legacy suppression");
    check(s.compressor_event(62000, true, false, false, true, true), "new cycle arms anew");
  }
  {
    auto s = started();
    check(!tick(s, 0, std::numeric_limits<float>::quiet_NaN()).apply && !s.owns_flow(), "NaN return cancels; normal limits take over");
    auto t = started();
    check(!tick(t, 0, std::numeric_limits<float>::infinity()).apply, "infinite return rejected");
    auto u = started();
    check(!tick(u, 0, 20.5f, std::numeric_limits<float>::infinity()).apply, "invalid normal AA rejected");
  }
  {
    for (const char *reason : {"Z2 demand ended", "defrost", "defrost recovery", "DHW", "lockout", "stale return", "ODIN takeover"}) {
      auto s = started();
      check(!tick(s, 1000, 20.5f, 26.0f, true, false).apply && !s.owns_flow(), reason);
      check(!s.compressor_event(2000, true, true, false, true, true), "no mid-cycle restart after cancellation");
      check(!s.compressor_event(3000, true, false, false, true, false), "ineligible post-defrost start refused");
    }
  }
  {
    auto s = started();
    tick(s, 0);
    check(near(tick(s, 899999).target, 21.5f), "active before timeout below minimum");
    check(near(tick(s, 899999, 23.0f).target, 24.0f), "minimum completes startup");
    s.resume_normal_aa();
    check(s.phase() == Phase::IDLE && !tick(s, 3720000).apply, "regular AA resumes without handover");
    check(!s.compressor_event(1600000, true, true, false, true, true), "completion does not restart in same cycle");
  }
  {
    auto s = started();
    check(!tick(s, 60000, 20.5f, 26.0f, false).apply && s.waiting_for_aa(),
          "switch OFF waits for regular AA without ramp");
  }
  {
    auto s = started();
    const auto requested = tick(s, 0);
    check(near(requested.target, 21.5f), "startup requests low value before guard");
    // Guards have the final say. Feedback must never start a downward correction loop.
    check(near(tick(s, 30000).target, 21.5f), "guard feedback does not ratchet startup request");
    check(near(tick(s, 900000).target, 21.5f), "timeout retains last startup target");
  }
  {
    auto s = started();
    auto r = s.calculate(0, true, true, 40.0f, 3.0f, 15.0f, 44.0f, 24.0f, 35.0f);
    check(near(r.target, 24.0f), "startup stops at minimum before configured maximum");
    auto t = started();
    check(near(tick(t, 0, 19.0f).target, 20.0f), "documented 20 C floor retained");
    auto u = started();
    check(!tick(u, 0, 19.0f, 19.5f).apply, "conflicting AA below technical floor falls back");
  }
  {
    auto s = started(0xfffffff0U);
    check(near(tick(s, 30000U).target, 21.5f), "millis wrap handled by unsigned elapsed time");
  }
  {
    auto s = started();
    check(near(s.calculate(0, true, true, 20.5f, 0, 60, 26, 24, 35).target, 20.5f), "zero offset accepted");
    check(near(s.calculate(30000, true, true, 20.5f, 0.1f, 60, 26, 24, 35).target, 20.6f), "0.1 K offset accepted");
    check(s.calculate(3599999, true, true, 20.5f, 0.1f, 60, 26, 24, 35).apply && s.owns_flow(), "60 minute duration remains active before expiry");
    auto r = s.calculate(3600000, true, true, 22.5f, 1, 60, 26, 24, 35);
    check(r.apply && near(r.target, 20.6f) && s.waiting_for_aa(), "60 minute expiry retains last request without ramp");
    check(!tick(s, 3630000).apply, "timeout stops extra startup writes");
    s.resume_normal_aa();
    check(s.phase() == Phase::IDLE && s.claimed_cycle(), "regular AA resumes while legacy ramps stay suppressed");
    auto t = started();
    t.calculate(0, true, true, 20, 1, 5, 23, 24, 35);
    check(t.calculate(300000, true, true, 20, 1, 5, 23, 24, 35).apply && t.waiting_for_aa(), "5 minute timeout works when AA cap prevents reaching minimum");
    auto u = started();
    u.calculate(0, true, true, 20, 1, 60, 26, 24, 35);
    check(near(u.calculate(60000, true, true, 23, 1, 60, 26, 24, 35).target, 24) && u.waiting_for_aa(), "minimum ends startup before 60 minute timer");
  }
  {
    auto s=started(0xfffffff0U);
    s.calculate(0xfffffff0U,true,true,20.5f,1,5,26,24,35);
    auto r=s.calculate(static_cast<uint32_t>(0xfffffff0U+300000U),true,true,20.5f,1,5,26,24,35);
    check(r.apply&&s.waiting_for_aa(),"timer expiry works across millis wrap");
    auto t=started();
    t.calculate(0,true,true,20.5f,1,60,26,24,35);
    check(t.calculate(600001,true,true,20.5f,1,5,26,24,35).apply&&t.waiting_for_aa(),"shortening duration during startup ends it");
    auto u=started();
    check(near(u.calculate(0,true,true,20.5f,-1,5,26,24,35).target,20.5f),"negative offset clamps to zero");
    auto v=started();
    check(near(v.calculate(0,true,true,20.5f,10,5,26,24,35).target,23.5f),"offset clamps to 3 K");
    auto w=started();
    check(near(w.calculate(0,true,true,20.5f,NAN,NAN,26,24,35).target,21.5f),"invalid configuration uses documented defaults");
    auto x=started();
    x.calculate(0,true,true,22.94f,1,15,26,24,35);
    check(x.owns_flow(),"rounded 23.9 target does not finish at 24");
    check(near(x.calculate(1000,true,true,22.96f,1,15,26,24,35).target,23.96f)&&x.waiting_for_aa(),"rounded 24.0 target finishes startup");
  }
  std::printf("PASS: %d checks (startup lifecycle, limits, minimum completion and regular AA resumption).\n", checks);
}
