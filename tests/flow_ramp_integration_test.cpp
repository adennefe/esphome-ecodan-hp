#define TEST_REAL_AA
#include "startup_control_integration_test.cpp"
static void samples(Optimizer &o,float hz=60){
  auto &s=o.instance.status;
  o.compressor_observed_ms_=o.wp_return_received_ms_=test_now;
  s.FlowRate=23;s.CompressorFrequency=static_cast<uint8_t>(hz);
  float v[]={hz,23,s.HpFeedTemperature,s.HpReturnTemperature,s.Z2FeedTemperature,s.Z2ReturnTemperature};
  for(unsigned i=0;i<GuardTelemetry::COUNT;++i)o.guard_telemetry_.observe(static_cast<GuardTelemetry::Field>(i),test_now,v[i]);
  o.z2_flow_ramp_.observe_frequency(test_now,hz);
}
static void prepare(Optimizer &o){
  o.flow_enabled.state=true;o.duration.state=45;o.room=o.room_target=19;o.zone_max=o.max_z2.state=31;
  o.compressor_observer_=CompressorStateTracker{};o.confirmed_compressor_run_=false;
  samples(o);o.register_compressor_observer_();o.instance.receive_compressor(false);samples(o);
}
static void fire(Optimizer &o){o.instance.receive_compressor(true);}
int main(){
  {
    Optimizer o;prepare(o);o.legacy_switch.state=true;fire(o);
    check(o.z2_flow_ramp_.active()&&near(o.live,22.5f),"real packet start writes return-at-start plus 2 before normal minimum");
    check(!o.z2_return_startup_.claimed_cycle()&&!o.z2_soft_start_pending_&&!o.z2_target_delta_soft_start_pending_,"new ramp has priority even when all old switches ON");
    check(o.processed[1]==1,"one AA calculation at compressor start");
    const int processed=o.processed[1];o.service_z2_flow_ramp();
    check(o.processed[1]==processed,"service cannot duplicate immediate AA cycle");
    o.instance.status.Z2ReturnTemperature=23;test_now+=30000;samples(o);o.service_z2_flow_ramp();
    check(near(o.live,22.5f)&&near(o.z2_flow_ramp_.base(),22.5f),"rising Z2 return does not lift base or cap");
    check(o.processed[0]==1,"extra startup service does not recalculate Z1");
    test_now+=270000;samples(o);o.run_auto_adaptive_loop(true);
    check(near(o.live,23),"production AA applies five minute ramp step");
    o.flow_enabled.state=false;o.service_z2_flow_ramp(true);
    check(!o.z2_flow_ramp_.active()&&near(o.live,29),"manual OFF immediately returns to normal 23+6 AA target");
    check(!o.z2_soft_start_pending_&&!o.z2_target_delta_soft_start_pending_,"no fallback old ramp mid-cycle");
  }
  for(float hz:{26.0f,16.0f}){
    Optimizer o;prepare(o);fire(o);const uint32_t start=test_now;
    for(unsigned m=0;m<=10;++m){test_now=start+m*60000;samples(o,hz);o.run_auto_adaptive_loop(true);}
    check(o.z2_flow_ramp_.holding()&&near(o.live,23),"production HOLD after five minutes at low frequency");
    o.instance.status.Z2ReturnTemperature=22.8f;test_now+=30000;samples(o,hz);o.service_z2_flow_ramp();
    check(near(o.live,23),"normal AA rise stays capped in HOLD");
    test_now+=30000;samples(o,35);o.service_z2_flow_ramp();
    check(!o.z2_flow_ramp_.holding()&&near(o.live,23.5f),"production frequency excursion releases HOLD");
    o.room=18.8f;test_now+=5000;samples(o,35);o.service_z2_flow_ramp();
    check(!o.z2_flow_ramp_.active()&&o.live>=24,"room deficit bypasses cap and restores normal min immediately");
  }
  {
    Optimizer o;prepare(o);fire(o);o.feed=o.instance.status.Z2FeedTemperature=25;o.tolerance.state=5;
    test_now+=30000;samples(o);o.run_auto_adaptive_loop(true);
    check(near(o.live,24)&&o.z2_flow_ramp_.active(),"real stop guard overrides 22.5 startup request to feed-1");
    test_now+=30000;samples(o);o.run_auto_adaptive_loop(true);
    check(!o.z2_flow_ramp_.active()&&near(o.live,26.5f),"second real stop guard returns to ordinary AA in same calculation");
    Optimizer b;prepare(b);fire(b);b.feed=b.instance.status.Z2FeedTemperature=29;
    test_now+=30000;samples(b);b.run_auto_adaptive_loop(true);
    check(near(b.live,29),"Buffer guard remains stronger than ramp");
    Optimizer x;prepare(x);fire(x);x.feed=x.instance.status.Z2FeedTemperature=40;
    test_now+=30000;samples(x);x.run_auto_adaptive_loop(true);
    check(near(x.live,31),"even guard override cannot exceed Z2 maximum");
  }
  {
    Optimizer o;prepare(o);fire(o);test_now+=45*60000;samples(o);o.service_z2_flow_ramp();
    check(!o.z2_flow_ramp_.active()&&near(o.live,26.5f),"duration ends without waiting for regular five minute AA");
    const int processed=o.processed[1];o.service_z2_flow_ramp();
    check(o.processed[1]==processed,"no extra writer after timeout handover");
    check(o.z2_flow_ramp_.claimed_cycle(),"cycle remains reserved until compressor stop");
    o.instance.receive_compressor(false);check(!o.z2_flow_ramp_.claimed_cycle(),"stop resets priority claim");
  }
  {
    Optimizer o;prepare(o);o.instance.receive_compressor(true);o.instance.receive_compressor(false);samples(o);fire(o);
    check(o.z2_flow_ramp_.active(),"next genuine start can use new ramp again");
    o.observe_compressor_state_(NAN);check(!o.z2_flow_ramp_.active(),"unknown compressor cancels ramp");
    Optimizer boot;prepare(boot);boot.compressor_observer_=CompressorStateTracker{};boot.instance.receive_compressor(true);
    check(!boot.z2_flow_ramp_.active()&&!boot.z2_flow_ramp_.claimed_cycle(),"ESP restart during running compressor cannot arm");
  }
  for(int mode=0;mode<10;++mode){
    Optimizer o;prepare(o);fire(o);auto &s=o.instance.status;
    switch(mode){
      case 0:s.DefrostActive=true;break;
      case 1:s.Operation=esphome::ecodan::Status::OperationMode::DHW_ON;break;
      case 2:s.MultiZoneStatus=0;break;
      case 3:s.FlowRate=9;o.guard_telemetry_.values[GuardTelemetry::FLOW_RATE]=9;break;
      case 4:s.Z2ReturnTemperature=NAN;break;
      case 5:o.guard_telemetry_.received[GuardTelemetry::Z2_FEED]=test_now-120001;break;
      case 6:o.last_defrost_time_=test_now-1;break;
      case 7:o.lockout.state=true;break;
      case 8:o.odin=true;break;
      case 9:o.aa=false;break;
    }
    o.service_z2_flow_ramp();check(!o.z2_flow_ramp_.active(),"unsafe condition cancels production ramp");
  }
  {
    Optimizer o;prepare(o);o.instance.status.HpReturnTemperature=25;samples(o);fire(o);
    check(near(o.live,22.5f),"anchor uses Z2 return 20.5, not primary WP return 25");
    o.instance.status.Z2ReturnTemperature=15;test_now+=30000;samples(o);o.service_z2_flow_ramp();
    check(near(o.live,21),"production normal AA below cap can reduce startup request");
    Optimizer boundary;prepare(boundary);boundary.instance.status.FlowRate=10;
    boundary.guard_telemetry_.values[GuardTelemetry::FLOW_RATE]=10;fire(boundary);
    check(boundary.z2_flow_ramp_.active(),"exact 10 l/min permits startup");
    Optimizer cold;prepare(cold);cold.room=18.8f;fire(cold);
    check(!cold.z2_flow_ramp_.active()&&!cold.z2_return_startup_.owns_flow(),"room deficit 0.2 K rejects startup and old-ramp fallback");
  }
  {
    Optimizer o;prepare(o);o.instance.status.FlowRate=9;fire(o);
    check(!o.z2_flow_ramp_.active()&&!o.z2_return_startup_.owns_flow(),"low flow cannot arm selected alternative or fall back to old startup");
  }
  std::printf("PASS: %d flow ramp production event/AA/guard/HOLD integration checks.\n",checks);
}
