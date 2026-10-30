#define TEST_REAL_AA
#include "startup_control_integration_test.cpp"
int main() {
  {
    Optimizer o;arm(o);
    o.run_auto_adaptive_loop(true);
    check(near(o.live,21.5f),"production AA write path applies return guided 21.5 instead of normal minimum 24");
    check(o.z1_writes==0&&o.processed[0]==0&&o.processed[1]==1,"extra cycle processes and writes Z2 only");
    check(o.stagnation_start_time_==0&&o.last_error_==0&&o.current_stagnation_boost_==1,"extra Z2 cycle preserves shared boost history");
    check(o.override_z1.changes==0&&o.override_z2.changes==0,"extra cycle leaves overrides alone");
    test_now+=60000;o.wp_return_received_ms_=test_now;o.instance.status.HpReturnTemperature=23;
    o.run_auto_adaptive_loop(true);
    check(o.z2_return_startup_.waiting_for_aa()&&near(o.live,24),"minimum reached writes 24 and enters wait");
    int writes=o.writes,processed=o.processed[1];o.run_auto_adaptive_loop(true);
    check(o.writes==writes&&o.processed[1]==processed,"extra AA call does nothing while waiting");
    o.predictive_boost_base_z2_setpoint_=21.5f;
    o.run_auto_adaptive_loop();
    check(o.z2_return_startup_.phase()==ReturnGuidedStartup::Phase::IDLE&&near(o.live,26.5f),"regular AA resumes normal 26.5 without a minute ramp");
    check(std::isnan(o.predictive_boost_base_z2_setpoint_),"normal AA discards stale protective boost base");
  }
  {
    Optimizer o;arm(o);o.duration.state=5;o.run_auto_adaptive_loop(true);
    test_now+=300000;o.wp_return_received_ms_=test_now;
    o.run_auto_adaptive_loop(true);
    check(o.z2_return_startup_.waiting_for_aa()&&near(o.live,21.5f),"production timeout holds low value below minimum");
    o.run_auto_adaptive_loop();
    check(near(o.live,26.5f),"regular AA resumes after timeout");
  }
  {
    Optimizer o;arm(o);o.instance.status.HpReturnTemperature=NAN;
    o.run_auto_adaptive_loop(true);
    check(!o.z2_return_startup_.owns_flow()&&near(o.live,26.5f),"invalid primary sensor cancels startup and uses normal valid zone AA");
  }
  {
    Optimizer o; o.run_auto_adaptive_loop();
    check(!o.z2_return_startup_.claimed_cycle()&&near(o.live,26.5f),"OFF retains normal AA flow");
  }
  std::printf("PASS: %d production AA scheduling/heating integration checks.\n",checks);
}
