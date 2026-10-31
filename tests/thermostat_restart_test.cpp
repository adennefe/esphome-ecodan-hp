#include <cstdio>
#include <cstdlib>
namespace esphome { namespace climate {
enum ClimateAction { CLIMATE_ACTION_OFF,CLIMATE_ACTION_IDLE,CLIMATE_ACTION_HEATING };
enum ClimateMode { CLIMATE_MODE_OFF,CLIMATE_MODE_HEAT,CLIMATE_MODE_HEAT_COOL };
} namespace thermostat {
class ThermostatClimate {
 public:
  bool supports_two_points_=false,supports_heat_=true;
  float target_temperature=19,target_temperature_low=19,current_temperature=19.4f;
  float heating_deadband_=0,heating_overrun_=0.5f;
  climate::ClimateAction action=climate::CLIMATE_ACTION_OFF;
  climate::ClimateMode mode=climate::CLIMATE_MODE_HEAT;
  bool heating_required_();
};
#include "actual_thermostat_heating.inc"
} }
int main(){
  using namespace esphome;thermostat::ThermostatClimate t;int checks=0;
  auto check=[&](bool ok,const char *msg){++checks;if(!ok){std::fprintf(stderr,"FAIL thermostat: %s\n",msg);std::exit(1);}};
  t.action=climate::CLIMATE_ACTION_HEATING;
  check(t.heating_required_(),"19.4 C preserves running heating with target19/overrun0.5");
  t.action=climate::CLIMATE_ACTION_OFF;
  check(!t.heating_required_(),"fresh action after reboot does not demand heating at 19.4 C");
  t.action=climate::CLIMATE_ACTION_IDLE;
  check(!t.heating_required_(),"idle action at 19.4 does not restart heating");
  t.current_temperature=19.0f;check(t.heating_required_(),"heat starts at <=19.0 C");
  t.current_temperature=19.5f;t.action=climate::CLIMATE_ACTION_HEATING;
  check(!t.heating_required_(),"running heating stops at >=19.5 C");
  t.current_temperature=19.49f;check(t.heating_required_(),"running heating persists below 19.5 C");
  std::printf("PASS: %d installed ESPHome thermostat hysteresis/restart checks.\n",checks);
}
