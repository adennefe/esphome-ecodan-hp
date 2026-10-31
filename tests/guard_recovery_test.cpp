#define TEST_GUARD_RECOVERY
#include "startup_control_integration_test.cpp"
static void sample(GuardTelemetry &s,uint32_t now,float feed=29.5f,float freq=26,float flow=23){
  s.observe(GuardTelemetry::FREQUENCY,now,freq);s.observe(GuardTelemetry::FLOW_RATE,now,flow);
  s.observe(GuardTelemetry::HP_FEED,now,feed);s.observe(GuardTelemetry::HP_RETURN,now,26);
  s.observe(GuardTelemetry::Z2_FEED,now,feed);s.observe(GuardTelemetry::Z2_RETURN,now,26);
}
int main(){
  {
    GuardRecovery r;GuardTelemetry s;float live=28.5f;
    for(uint32_t t=0;t<=600000;t+=30000){sample(s,t);auto v=r.update(t,true,27,28.5f,28.5f,live,s);live=v.target;}
    check(near(live,28.5f),"stable 26 Hz alone never permits dropping below existing feed-minus-1 floor");
  }
  {
    GuardRecovery r;GuardTelemetry s;float live=29;
    GuardRecovery::Result v{live,2};
    for(uint32_t t=0;t<=600000;t+=30000){sample(s,t,29.5f);v=r.update(t,true,27,28.5f,live,live,s);live=v.target;}
    check(v.step&&near(live,28.5f),"stable held uplift reduced by at most 0.5 K within guard floor");
    sample(s,630000,29.4f);v=r.update(630000,true,27,28.4f,live,live,s);
    check(!v.step&&near(v.target,28.5f),"repeated callbacks cannot advance recovery clock");
    sample(s,660000,29.8f,40);v=r.update(660000,true,27,28.8f,29,28.5f,s);
    check(near(v.target,29)&&!v.permitted,"frequency instability restores legacy guard immediately");
    sample(s,700000,30);v=r.update(700000,false,27,29,29.5f,29,s);
    check(near(v.target,29.5f),"ineligible/disabled recovery is exactly the legacy target");
  }
  {
    Optimizer o;o.guard_enable.state=false;o.feed=29;o.live=28.5f;
    check(near(o.apply_z2_guard_recovery_(28,true,27,2.5f),28),"switch OFF does not alter AA result");
    o.guard_enable.state=true;o.confirmed_compressor_run_=true;o.confirmed_compressor_start_ms_=test_now-1800000;
    sample(o.guard_telemetry_,test_now,29);o.instance.status.Z2ReturnTemperature=26;o.feed=29;
    o.apply_z2_guard_recovery_(28.5f,true,27,2.5f);
    auto target=o.apply_z2_guard_recovery_(28.5f);
    check(target>=28,"production recovery never undercuts current legacy guard floor");
    o.instance.status.DefrostActive=true;
    check(near(o.apply_z2_guard_recovery_(29),29),"defrost falls back to original guard target");
    o.instance.status.DefrostActive=false;o.instance.status.Operation=esphome::ecodan::Status::OperationMode::DHW_ON;
    check(near(o.apply_z2_guard_recovery_(29),29),"DHW falls back to original guard target");
    o.instance.status.Operation=esphome::ecodan::Status::OperationMode::HEAT_ON;o.z2_return_startup_.compressor_event(test_now,true,false,false,true,true);
    check(near(o.apply_z2_guard_recovery_(21.5f),21.5f),"no competing guard recovery during startup");
  }
  {
    GuardRecovery r;GuardTelemetry s;sample(s,0);
    check(near(r.update(120001,true,27,28.5f,29,29,s).target,29),"stale measurements cannot authorize recovery");
    sample(s,150000,29.5f,26,0);
    check(near(r.update(150000,true,27,28.5f,29,29,s).target,29),"zero flow cannot authorize recovery");
  }
  {
    GuardRecovery r;GuardTelemetry s;float live=28.5f;int steps=0;
    for(uint32_t t=0;t<=1800000;t+=30000){
      sample(s,t,28,20);auto v=r.update(t,true,27,27,live,live,s);
      if(v.step){++steps;check(v.target>=live-0.501f,"each recovery step at most 0.5 K");}
      check(v.target>=27,"every target respects current strict floor and normal AA");live=v.target;
    }
    check(steps==3&&near(live,27),"held 1.5 K uplift decays in three ten-minute steps");
    sample(s,1830000,29,20);auto v=r.update(1830000,true,27,28,28,27,s);
    check(near(v.target,28)&&!v.step,"new guard need after completed decay immediately takes precedence");
  }
  {
    GuardRecovery r;GuardTelemetry s;sample(s,0,28,20);
    r.update(0,true,27,27,28.5f,28.5f,s);
    sample(s,600000,28,20);auto v=r.update(600000,true,27,27,28.5f,28.5f,s);
    check(!v.step,"ten-minute gap is not ten minutes of observed stability");
    for(uint32_t t=630000;t<=1200000;t+=30000){sample(s,t,28,20);v=r.update(t,true,27,27,28.5f,28.5f,s);}
    check(v.step,"continuous observations after gap permit bounded recovery");
  }
  {
    GuardRecovery r;GuardTelemetry s;GuardRecovery::Result v{28.5f,1.5f};
    for(uint32_t t=0;t<=600000;t+=30000){sample(s,t,t<600000?28.0f:28.3f,20);v=r.update(t,true,27,27,28.5f,28.5f,s);}
    check(!v.step,"rising primary or Z2 feed blocks recovery despite low frequency");
    sample(s,630000,28,20);s.observe(GuardTelemetry::Z2_RETURN,630000,NAN);
    check(!r.update(630000,true,27,27,28.5f,28.5f,s).permitted,"invalid Z2 return blocks recovery");
  }
  {
    GuardRecovery r;GuardTelemetry s;GuardRecovery::Result v{28.5f,1.5f};
    for(uint32_t t=0;t<=600000;t+=30000){sample(s,t,28,20);v=r.update(t,true,27,27,28.5f,29,s);}
    check(!v.step,"unconfirmed/externally changed setpoint blocks recovery");
    const uint32_t origin=0xfffffff0U;GuardRecovery w;float live=28.5f;
    for(uint32_t t=0;t<=600000;t+=30000){sample(s,origin+t,28,20);v=w.update(origin+t,true,27,27,live,live,s);live=v.target;}
    check(v.step&&near(live,28),"recovery timing works across millis wrap");
  }
  {
    Optimizer o;o.guard_enable.state=true;o.confirmed_compressor_run_=true;o.confirmed_compressor_start_ms_=test_now-1800000;
    o.feed=29.5f;o.live=28.5f;o.tolerance.state=5;
    o.instance.status.Z2ReturnTemperature=26;o.instance.status.HpReturnTemperature=26;
    o.instance.status.CompressorFrequency=20;
    for(uint32_t elapsed=0;elapsed<=600000;elapsed+=30000){
      test_now+=30000;o.wp_return_received_ms_=o.compressor_observed_ms_=test_now;
      o.feed=elapsed<600000?29.5f:29.0f;
      sample(o.guard_telemetry_,test_now,o.feed,20);
      if(elapsed%300000==0){
        const float original=o.apply_flow_limits(OptimizerZone::ZONE_2,27,2.5f,26);
        const float command=o.apply_z2_guard_recovery_(original,true,27,2.5f);
        o.set_flow_temp(command,OptimizerZone::ZONE_2);
      }
      const float command=o.apply_z2_guard_recovery_(o.live);o.set_flow_temp(command,OptimizerZone::ZONE_2);
    }
    check(near(o.live,28),"production wrapper unwinds uplift only as original guard floor falls");
    o.guard_enable.state=false;
    check(near(o.apply_z2_guard_recovery_(27.2f),27.2f),"disabling after recovery gives exact legacy result");
  }
  std::printf("PASS: %d conservative guard recovery checks.\n",checks);
}
