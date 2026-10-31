#define TEST_COMPRESSOR
#include "startup_control_integration_test.cpp"
static void fresh(Optimizer &o){o.wp_return_received_ms_=test_now;}
static void unknown(Optimizer &o){o.compressor_observer_=CompressorStateTracker{};o.confirmed_compressor_run_=false;}
int main(){
  {
    Instance d;Binary compressor,defrost;
    d.register_binarySensor(&compressor,"status_compressor");
    d.register_binarySensor(&defrost,"status_defrost");
    check(!compressor.has_state(),"compressor entity stays UNKNOWN until first real packet");
    check(defrost.has_state()&&!defrost.state,"other existing status entity defaults unchanged");
  }
  {
    Optimizer o;unknown(o);o.register_compressor_observer_();
    check(!o.compressor_observer_.known(),"synthetic entity OFF is not evidence of real compressor OFF");
    o.instance.receive_compressor(true);
    check(o.compressor_observer_.running()&&!o.z2_return_startup_.owns_flow(),"boot while running: first real ON does not start startup");
    check(!o.confirmed_compressor_run_&&o.compressor_start_time_==0,"initial ON is not an artificial compressor runtime");
    check(o.aa_calls==0,"initial ON does not arm old ramps or trigger start AA");
  }
  {
    Optimizer o;unknown(o);o.register_compressor_observer_();
    o.instance.receive_compressor(false);o.instance.receive_compressor(false);
    check(o.compressor_observer_.known()&&!o.compressor_observer_.running(),"real OFF seeds observer despite entity deduplication");
    test_now+=60000;fresh(o);o.instance.receive_compressor(true);
    check(o.z2_return_startup_.owns_flow()&&o.confirmed_compressor_run_,"first confirmed OFF->ON arms startup");
    const auto start=o.confirmed_compressor_start_ms_;o.instance.receive_compressor(true);
    check(start==o.confirmed_compressor_start_ms_,"repeated ON does not reset runtime");
    o.instance.receive_compressor(false);
    check(!o.z2_return_startup_.owns_flow()&&!o.confirmed_compressor_run_,"real stop resets startup");
    fresh(o);o.instance.receive_compressor(true);check(o.z2_return_startup_.owns_flow(),"next real start available again");
  }
  {
    Optimizer o;unknown(o);o.instance.receive_compressor(true);o.register_compressor_observer_();
    check(o.compressor_observer_.running()&&!o.z2_return_startup_.owns_flow(),"late subscriber seeds real running state without start");
    Optimizer p;unknown(p);p.instance.receive_compressor(false);p.register_compressor_observer_();
    fresh(p);p.instance.receive_compressor(true);check(p.z2_return_startup_.owns_flow(),"late subscriber seeds real OFF then catches start");
  }
  {
    Optimizer o;unknown(o);o.register_compressor_observer_();
    o.instance.receive_compressor(false);o.observe_compressor_state_(NAN);o.instance.receive_compressor(true);
    check(!o.z2_return_startup_.owns_flow(),"unknown interval cannot manufacture an OFF->ON edge");
    o.instance.receive_compressor(false);fresh(o);o.instance.receive_compressor(true);
    check(o.z2_return_startup_.owns_flow(),"confirmed OFF after invalid state restores start eligibility");
    o.observe_compressor_state_(0.5f);check(!o.compressor_observer_.known(),"invalid non-binary value rejected");
  }
  {
    Optimizer o;unknown(o);o.enabled.state=false;o.register_compressor_observer_();
    o.instance.receive_compressor(false);o.instance.receive_compressor(true);
    check(!o.z2_return_startup_.claimed_cycle(),"feature OFF retains ordinary event handling");
    o.enabled.state=true;o.instance.receive_compressor(true);
    check(!o.z2_return_startup_.claimed_cycle(),"enabling mid-cycle does not fabricate a start");
  }
  {
    Optimizer o;unknown(o);o.register_compressor_observer_();o.instance.receive_compressor(false);
    for(int i=0;i<10;++i){test_now+=60000;fresh(o);o.instance.receive_compressor(false);}
    o.instance.receive_compressor(true);check(o.z2_return_startup_.owns_flow(),"repeated raw OFF packets keep reliable evidence fresh despite entity deduplication");
    test_now+=120001;fresh(o);o.instance.receive_compressor(true);
    check(!o.z2_return_startup_.owns_flow()&&!o.confirmed_compressor_run_,"packet gap cannot hide a restart and retain confirmed continuity");
    o.instance.receive_compressor(false);o.instance.receive_compressor(true);
    check(o.z2_return_startup_.owns_flow(),"new known OFF->ON after packet gap recovers start detection");
  }
  std::printf("PASS: %d real packet/observer/start-event regression checks.\n",checks);
}
