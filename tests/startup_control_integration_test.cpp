// Production eligibility, service and predictive-guard methods with device fixtures.
#include "components/optimizer/return_guided_startup.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include "components/ecodan/proto.h"
#include "components/ecodan/status.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using std::isnan;
static uint32_t test_now = 4000000;
static uint32_t millis() { return test_now; }
namespace esphome { namespace number { struct Number { float state; }; } namespace optimizer {
enum class OptimizerZone { SINGLE, ZONE_1, ZONE_2 };
using Number = esphome::number::Number;
struct Switch { bool state; int changes=0; void turn_on(){state=true;++changes;} void turn_off(){state=false;++changes;} };
struct Binary { bool state{false}, observed{false}; bool has_state() const { return observed; }
  void publish_state(bool v) { state=v; observed=true; } };
struct Select { int index=0; std::optional<int> active_index() const { return index; } };
struct FlowLimits { float min, max; };
struct HeatingProfile { float base_min_delta_t, min_delta_cold_limit, max_delta_t, max_error_range, defrost_memory_ms; };
struct Instance { ecodan::Status status{}; ecodan::Status &get_status() { return status; } };
class Optimizer {
 public:
  Instance instance;
  Number tolerance{0}, cool_min{18}, cool_start{19}; Select profile;
  Switch enabled{true}, aa_switch{true}, defrost_switch{false}, legacy_switch{false}, override_z1{false}, override_z2{false}; Binary active, lockout;
  Number hp_feed{21}, min_z1{24}, max_z1{35}, min_z2{24}, max_z2{35}, increment{0.5f}, interval{5};
  struct State {
    Instance *ecodan_instance;
    Select *heating_system_type;
    Switch *z2_return_guided_startup;
    Binary *z2_startup_active, *status_short_cycle_lockout;
    Number *z1_guard_delta_t_tolerance, *z2_guard_delta_t_tolerance;
    Number *minimum_cooling_flow_temp_z1, *minimum_cooling_flow_temp_z2, *cooling_smart_start_temp;
    Switch *sw_odin_override_z1=nullptr, *sw_odin_override_z2=nullptr, *defrost_risk_handling_enabled=nullptr, *z2_soft_start_enabled=nullptr;
    Number *hp_feed_temp=nullptr, *maximum_heating_flow_temp=nullptr, *minimum_heating_flow_temp=nullptr,
           *maximum_heating_flow_temp_z2=nullptr, *minimum_heating_flow_temp_z2=nullptr,
           *z2_soft_start_step=nullptr, *z2_soft_start_interval_minutes=nullptr, *z2_startup_return_offset=nullptr, *z2_startup_duration=nullptr;
  } state_{&instance,&profile,&enabled,&active,&lockout,&tolerance,&tolerance,&cool_min,&cool_min,&cool_start};
  ReturnGuidedStartup z2_return_startup_;
  bool aa=true, odin=false, post_dhw=false, wp_return_observed_=true;
  uint32_t last_defrost_time_=0, wp_return_received_ms_=test_now, return_startup_service_ms_=0;
  float room=20, room_target=21, feed=21, live=24.0f, zone_min=24, zone_max=35;
  float predictive_boost_base_z1_setpoint_=NAN, predictive_boost_base_z2_setpoint_=NAN;
  int aa_calls=0, writes=0, z1_writes=0; bool extra=false;
  int processed[2]={0,0};
  Number offset{1},duration{15};
  bool adaptive_loop_running_=false;
  uint32_t stagnation_start_time_=0;
  float last_error_=0, current_stagnation_boost_=1;
  bool z2_soft_start_pending_=false,z2_soft_start_active_=false;
  uint32_t z2_soft_start_start_ms_=0;
  int32_t z2_soft_start_last_step_=-1;
  float z2_soft_start_initial_flow_=NAN,z2_soft_start_floor_=NAN;
  struct DefrostState { float locked_outside_temp_=NAN; float get_return_temp(bool,OptimizerZone) { return NAN; } };
  DefrostState resolve_defrost_state_() { return {}; }
  float get_return_temp(OptimizerZone z) { return z==OptimizerZone::ZONE_2 ? instance.status.Z2ReturnTemperature : instance.status.Z1ReturnTemperature; }
  float apply_z2_target_delta_soft_start_(float v,const ecodan::Status &,int) { return v; }
  void reset_z2_soft_start_(const char *) { z2_soft_start_pending_=z2_soft_start_active_=false; }
  float calculate_heating_flow_(std::size_t,const ecodan::Status &,const HeatingProfile &,bool,float,float,float,float,int);
  void process_adaptive_zone_(std::size_t i,const ecodan::Status &status,const HeatingProfile &p,float,float outside,float high,float low,float &heat,float &) {
    ++processed[i];stagnation_start_time_=999;last_error_=2;current_stagnation_boost_=3;
    heat=calculate_heating_flow_(i,status,p,false,outside,low,high,6,0);
  }
  const float MAX_FEED_STEP_CHANGE=1, MAX_FEED_STEP_ADJUSTMENT=1;
  Optimizer() {
    state_.sw_odin_override_z1=&override_z1;state_.sw_odin_override_z2=&override_z2;
    state_.hp_feed_temp=&hp_feed;state_.defrost_risk_handling_enabled=&defrost_switch;
    state_.z2_soft_start_enabled=&legacy_switch;state_.z2_soft_start_step=&increment;state_.z2_soft_start_interval_minutes=&interval;
    state_.minimum_heating_flow_temp=&min_z1;state_.maximum_heating_flow_temp=&max_z1;
    state_.minimum_heating_flow_temp_z2=&min_z2;state_.maximum_heating_flow_temp_z2=&max_z2;
    state_.z2_startup_return_offset=&offset;state_.z2_startup_duration=&duration;
    auto &s=instance.status;
    s.CompressorOn=true; s.CompressorFrequency=30;
    s.DipSwitch2=(1<<5)|(1<<6); s.DipSwitch3=0;
    s.Operation=ecodan::Status::OperationMode::HEAT_ON;
    s.HeatingCoolingMode=s.HeatingCoolingModeZone2=ecodan::Status::HpMode::HEAT_FLOW_TEMP;
    s.MultiZoneStatus=3; s.HpReturnTemperature=20.5f; s.HpFeedTemperature=21;
    s.Z1ReturnTemperature=20.5f;s.Z1FeedTemperature=21;s.Zone1FlowTemperatureSetPoint=24;s.Zone2FlowTemperatureSetPoint=24;
    s.Z2FeedTemperature=21; s.Z2ReturnTemperature=20.5f; s.OutsideTemperature=7;
  }
  bool aa_enabled() const { return aa; }
  bool odin_forwarder_takeover_active() const { return odin; }
  bool is_post_dhw_window(const ecodan::Status &) { return post_dhw; }
  bool is_system_hands_off(const ecodan::Status &s) { return s.DefrostActive || lockout.state || s.Operation!=ecodan::Status::OperationMode::HEAT_ON; }
  bool is_compressor_active(const ecodan::Status &s) { return s.CompressorOn && s.CompressorFrequency>0; }
  bool is_cooling_mode(const ecodan::Status &s, OptimizerZone) { return s.is_cooling_active(); }
  float get_room_current_temp(OptimizerZone) { return room; }
  float get_room_target_temp(OptimizerZone) { return room_target; }
  float get_feed_temp(OptimizerZone) { return feed; }
  float get_flow_setpoint(OptimizerZone) { return live; }
  FlowLimits get_flow_limits(OptimizerZone) { return {zone_min,zone_max}; }
  FlowLimits get_cool_flow_limits(OptimizerZone) { return {18,25}; }
  float round_nearest(float v) { return std::round(v*10)/10; }
  float clamp_flow_temp(float v,float low,float high) { return std::clamp(v,low,high); }
  bool set_flow_temp(float v,OptimizerZone z) { if(z==OptimizerZone::ZONE_1){++z1_writes;instance.status.Zone1FlowTemperatureSetPoint=v;return true;} if (std::fabs(v-live)<0.05f) return false; live=v; instance.status.Zone2FlowTemperatureSetPoint=v; ++writes; return true; }
  void clear_predictive_boost_(OptimizerZone z,bool) { (z==OptimizerZone::ZONE_2 ? predictive_boost_base_z2_setpoint_:predictive_boost_base_z1_setpoint_)=NAN; }
#ifdef TEST_REAL_AA
  void run_auto_adaptive_loop(bool value=false);
#else
  void run_auto_adaptive_loop(bool value=false) { ++aa_calls; extra=value; }
#endif
  HeatingProfile get_heating_profile_(int);
  const char *return_startup_ineligible_reason_(const ecodan::Status &);
  void publish_return_guided_startup_();
  void cancel_return_guided_startup_(const char *);
  void service_return_guided_startup(bool force=false);
  float enforce_step_limit(const ecodan::Status &,float,float,bool);
  float apply_flow_limits(OptimizerZone,float,float,float,float=NAN);
  void predictive_short_cycle_check_for_zone_(const ecodan::Status &,OptimizerZone,bool);
};
#include "actual_flow_limits.inc"
#include "actual_profile.inc"
#include "actual_predictive.inc"
#include "actual_heating.inc"
#ifdef TEST_REAL_AA
#include "actual_aa_loop.inc"
#endif
} }
#include "actual_startup_control.inc"
using namespace esphome::optimizer;
static int checks=0;
static void check(bool ok,const char *msg) { ++checks; if(!ok){std::fprintf(stderr,"FAIL control integration: %s\n",msg);std::exit(1);} }
static bool near(float a,float b){return std::fabs(a-b)<0.001f;}
static void arm(Optimizer &o) { check(o.z2_return_startup_.compressor_event(test_now,true,false,false,true,true),"arm fixture"); }
#ifndef TEST_REAL_AA
int main() {
  // Unlike the old eligibility=false parameter tests, these execute the real
  // eligibility function AND real Ecodan status predicates for every case.
  {
    Optimizer o; auto &s=o.instance.status;
    check(!o.return_startup_ineligible_reason_(s),"valid measured Z2 heating eligible");
    for(int mz=0;mz<4;++mz){s.MultiZoneStatus=mz;check((o.return_startup_ineligible_reason_(s)==nullptr)==(mz==1||mz==3),"actual multizone status eligibility");}
    s.MultiZoneStatus=3; o.aa=false; check(o.return_startup_ineligible_reason_(s),"AA off"); o.aa=true;
    o.odin=true;check(o.return_startup_ineligible_reason_(s),"ODIN takeover");o.odin=false;
    s.CompressorOn=false;check(o.return_startup_ineligible_reason_(s),"compressor stopped");s.CompressorOn=true;
    s.DipSwitch2=0;check(o.return_startup_ineligible_reason_(s),"shared or single zone rejected");s.DipSwitch2=(1<<5)|(1<<6);
    s.HeatingCoolingMode=esphome::ecodan::Status::HpMode::HEAT_ROOM_TEMP;check(o.return_startup_ineligible_reason_(s),"Z1 writer gate");s.HeatingCoolingMode=esphome::ecodan::Status::HpMode::HEAT_FLOW_TEMP;
    s.HeatingCoolingModeZone2=esphome::ecodan::Status::HpMode::HEAT_ROOM_TEMP;check(o.return_startup_ineligible_reason_(s),"Z2 wrong mode");s.HeatingCoolingModeZone2=esphome::ecodan::Status::HpMode::HEAT_FLOW_TEMP;
    s.ServerControl=true;s.ProhibitHeatingZ2=true;check(o.return_startup_ineligible_reason_(s),"Z2 prohibited by actual status predicate");s.ProhibitHeatingZ2=false;
    s.DefrostActive=true;check(o.return_startup_ineligible_reason_(s),"active defrost");s.DefrostActive=false;
    for(int index=0;index<6;++index){o.profile.index=index;uint32_t recovery=index<2?2100000:index<4?1500000:900000;o.last_defrost_time_=test_now-recovery+1;check(o.return_startup_ineligible_reason_(s),"profile recovery rejects start just before boundary");o.last_defrost_time_=test_now-recovery;check(!o.return_startup_ineligible_reason_(s),"profile recovery allows start at boundary");}o.last_defrost_time_=0;
    s.Operation=esphome::ecodan::Status::OperationMode::DHW_ON;check(o.return_startup_ineligible_reason_(s),"DHW");s.Operation=esphome::ecodan::Status::OperationMode::COOL_ON;check(o.return_startup_ineligible_reason_(s),"cooling");s.Operation=esphome::ecodan::Status::OperationMode::HEAT_ON;
    o.post_dhw=true;check(o.return_startup_ineligible_reason_(s),"post DHW");o.post_dhw=false;
    o.lockout.state=true;check(o.return_startup_ineligible_reason_(s),"lockout");o.lockout.state=false;
    o.wp_return_received_ms_=test_now-120000;check(!o.return_startup_ineligible_reason_(s),"return age exactly 120 seconds");--o.wp_return_received_ms_;check(o.return_startup_ineligible_reason_(s),"stale return rejected");o.wp_return_received_ms_=test_now;
    s.HpReturnTemperature=NAN;check(o.return_startup_ineligible_reason_(s),"primary return NaN");s.HpReturnTemperature=20.5f;
    s.Z2ReturnTemperature=NAN;check(o.return_startup_ineligible_reason_(s),"Z2 return NaN");s.Z2ReturnTemperature=20.5f;
    o.room=NAN;check(o.return_startup_ineligible_reason_(s),"room sensor NaN");
  }
  {
    Optimizer o;arm(o);o.return_startup_service_ms_=test_now;
    o.service_return_guided_startup();check(o.aa_calls==0,"no duplicate immediate service");
    test_now+=30000;o.wp_return_received_ms_=test_now;o.service_return_guided_startup();check(o.aa_calls==1&&o.extra,"30 second service updates only Z2");
    o.enabled.state=false;o.service_return_guided_startup(true);check(o.z2_return_startup_.waiting_for_aa()&&!o.active.state&&o.aa_calls==1,"manual OFF waits without extra AA calculation");
  }
  {
    Optimizer o;arm(o);o.z2_return_startup_.calculate(test_now,true,true,20.5f,1,5,26,24,35);
    o.z2_return_startup_.calculate(test_now+300000,true,true,20.5f,1,5,26,24,35);
    check(o.z2_return_startup_.waiting_for_aa(),"timeout enters wait");
    o.feed=25;o.live=21.5f;
    o.predictive_short_cycle_check_for_zone_(o.instance.status,OptimizerZone::ZONE_2,false);
    check(near(o.live,22)&&o.writes==1,"stop guard continues during wait with original 0.5 K protective step");
    o.feed=21.5f;o.live=21.5f;o.writes=0;
    o.predictive_short_cycle_check_for_zone_(o.instance.status,OptimizerZone::ZONE_2,false);
    check(near(o.live,21.5f)&&o.writes==0,"wait does not restore 24 C minimum early");
  }
  {
    Optimizer o;arm(o);o.z2_return_startup_.wait_for_aa();o.wp_return_received_ms_=test_now-120001;
    o.predictive_boost_base_z2_setpoint_=21.5f;o.service_return_guided_startup();
    check(!o.z2_return_startup_.waiting_for_aa()&&std::isnan(o.predictive_boost_base_z2_setpoint_),"invalid sensor during wait cancels and drops stale boost base");
    check(o.aa_calls==0,"cancelling wait does not invent an AA cycle");
    Optimizer z;arm(z);z.feed=25;z.live=21.5f;z.predictive_short_cycle_check_for_zone_(z.instance.status,OptimizerZone::ZONE_1,false);
    check(near(z.instance.status.Zone1FlowTemperatureSetPoint,24),"Z1 predictive minimum unchanged during Z2 startup");
  }
  std::printf("PASS: %d production eligibility/service/predictive integration checks.\n",checks);
}

#endif
