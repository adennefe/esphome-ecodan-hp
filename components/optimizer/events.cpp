#include "optimizer.h"

using std::isnan;

namespace esphome
{
    namespace optimizer
    {
        using namespace esphome::ecodan;

        void Optimizer::reset_z2_soft_start_(const char *reason)
        {
            if (this->z2_soft_start_pending_ || this->z2_soft_start_active_)
                ESP_LOGI(OPTIMIZER_TAG, "Z2 soft-start cancelled: %s.", reason);
            this->z2_soft_start_pending_ = false;
            this->z2_soft_start_active_ = false;
            this->z2_soft_start_last_step_ = -1;
            this->z2_soft_start_initial_flow_ = NAN;
            this->z2_soft_start_floor_ = NAN;
        }

        void Optimizer::cancel_z2_soft_start()
        {
            this->reset_z2_soft_start_("disabled by user");
        }

        void Optimizer::reset_z2_target_delta_soft_start_(const char *reason)
        {
            if (this->z2_target_delta_soft_start_pending_ || this->z2_target_delta_soft_start_active_)
                ESP_LOGI(OPTIMIZER_TAG, "Z2 target-delta soft-start cancelled: %s.", reason);
            this->z2_target_delta_soft_start_pending_ = false;
            this->z2_target_delta_soft_start_active_ = false;
            this->z2_target_delta_soft_start_reached_raw_ = false;
            this->z2_target_delta_last_step_ = -1;
            this->z2_target_delta_initial_raw_ = NAN;
            this->z2_target_delta_max_raw_seen_ = NAN;
            this->z2_target_delta_start_value_ = NAN;
            this->z2_target_delta_ramp_value_ = NAN;
        }

        void Optimizer::cancel_z2_target_delta_soft_start()
        {
            this->reset_z2_target_delta_soft_start_("disabled by user");
        }

        void Optimizer::on_feed_temp_change(float actual_flow_temp, OptimizerZone zone) {
            if (std::isnan(actual_flow_temp))
                return;

            this->predictive_short_cycle_check();
            this->handle_dhw_feed_temp_(actual_flow_temp, zone);
        }

        // callbacks to monitor step down, need to keep within 1.0C else compressor will halt
        void Optimizer::handle_dhw_feed_temp_(float actual_flow_temp, OptimizerZone zone) {
            auto &status = this->state_.ecodan_instance->get_status();
            bool post_dhw_window = this->is_post_dhw_window(status);
            if (!this->is_dhw_active(status) && !post_dhw_window)
                return;

            if ((this->state_.status_short_cycle_lockout != nullptr && this->state_.status_short_cycle_lockout->state)
                || (this->state_.auto_adaptive_control_enabled != nullptr && !this->state_.auto_adaptive_control_enabled->state)) {
                return;
            }


            if (status.has_independent_zone_temps()) 
            {
                if (zone == OptimizerZone::SINGLE) {
                    return;
                }
            } 
            else 
            {
                if (zone != OptimizerZone::SINGLE) {
                    return;
                }
            }

            if (this->is_cooling_mode(status, zone))
            {
                this->clear_dhw_follow_state_(status, zone);
                return;
            }

            float current_flow_setpoint = (zone == OptimizerZone::ZONE_2) ? status.Zone2FlowTemperatureSetPoint : status.Zone1FlowTemperatureSetPoint;
            float adjusted_flow = actual_flow_temp;

            time_t current_timestamp = status.timestamp();

            if (this->is_dhw_active(status)) {
                // Trail already when nearly at the DHW target: within 1 °C of it.
                if (std::isnan(status.DhwFlowTemperatureSetPoint) ||
                    (status.DhwFlowTemperatureSetPoint - actual_flow_temp) > 1.0f)
                    return;

                float saved_setpoint = (zone == OptimizerZone::ZONE_2) ? this->dhw_old_z2_setpoint_ : this->dhw_old_z1_setpoint_;
                if (std::isnan(saved_setpoint))
                    return;

                adjusted_flow += 0.5f;
                // Each time we adjust for dhw, set the post dhw timer expiration
                time_t current_timestamp = status.timestamp();
                if (status.CompressorOn && current_timestamp > 0) {
                    const uint32_t after_dhw_monitoring_duration_s = 5 * 60UL;
                    this->dhw_post_run_expiration_ = (uint32_t)(current_timestamp + after_dhw_monitoring_duration_s);
                    ESP_LOGD(OPTIMIZER_TAG, "Setting monitor expiration to: %lu", this->dhw_post_run_expiration_);
                }
            }
            else if (post_dhw_window) {
                if (!status.is_heating_active() && !status.is_cooling_active()) {
                    // no demand, restore saved setpoint
                    float restore_val = NAN;
                    if (zone == OptimizerZone::ZONE_2) {
                        restore_val = this->dhw_old_z2_setpoint_;
                        this->dhw_old_z2_setpoint_ = NAN;
                    } else {
                        restore_val = this->dhw_old_z1_setpoint_;
                        this->dhw_old_z1_setpoint_ = NAN;
                        if (!status.has_2zones())
                            this->dhw_old_z2_setpoint_ = NAN;
                    }

                    if (std::isnan(restore_val))
                        return;

                    ESP_LOGD(OPTIMIZER_TAG, "Post-DHW: Heat/Cool demand gone. Restoring original setpoint Z%d: %.1f.", static_cast<uint8_t>(zone), restore_val);

                    adjusted_flow = restore_val;
                    if (std::isnan(this->dhw_old_z1_setpoint_) && std::isnan(this->dhw_old_z2_setpoint_))
                        this->dhw_post_run_expiration_ = 0;
                }
                else if (status.is_heating_active())
                {
                    // Trail only the DHW -> heating handover
                    if (fabs(actual_flow_temp - current_flow_setpoint) < 1.0f)
                        return;   // no stop risk: leave the setpoint alone

                    float adjusted_target = actual_flow_temp - 0.5f;

                    ESP_LOGW(OPTIMIZER_TAG, "Heating/DHW flow adjust: %.2f°C to prevent compressor stop! (setpoint: %.2f°C is %.2f°C below actual feed temp)",
                            adjusted_target, current_flow_setpoint, (actual_flow_temp - current_flow_setpoint));

                    adjusted_flow = adjusted_target;
                }
                else
                {
                    // Neither heating nor cooling runs this zone (manual
                    // cooling without auto-adaptive, defrost): the following
                    // does not apply, hands off.
                    return;
                }
            }   
            else {
                bool cooling_mode = is_cooling_mode(status, zone);
                adjusted_flow = enforce_step_limit(status, actual_flow_temp, current_flow_setpoint, cooling_mode);
            }
    
            if (adjusted_flow != current_flow_setpoint)
            {
                set_flow_temp(adjusted_flow, zone);
            }
        }

        // Drop the DHW feed-temp following state: the saved (heating) flow
        // setpoints and, once both are gone, the post-DHW window - the same
        // closing the post-DHW restore does.
        void Optimizer::clear_dhw_follow_state_(const ecodan::Status &status, OptimizerZone zone)
        {
            if (zone == OptimizerZone::ZONE_2)
                this->dhw_old_z2_setpoint_ = NAN;
            else
            {
                this->dhw_old_z1_setpoint_ = NAN;
                if (!status.has_2zones())
                    this->dhw_old_z2_setpoint_ = NAN;
            }

            if (std::isnan(this->dhw_old_z1_setpoint_) && std::isnan(this->dhw_old_z2_setpoint_))
                this->dhw_post_run_expiration_ = 0;
        }

        bool Optimizer::set_flow_temp(float flow, OptimizerZone zone) {
            if (zone == OptimizerZone::ZONE_2 && this->z2_return_startup_.claimed_cycle() && !std::isfinite(flow)) {
                ESP_LOGW(OPTIMIZER_TAG, "Z%d refusing non-finite flow command.", static_cast<int>(zone));
                return false;
            }
            auto &status = this->state_.ecodan_instance->get_status();

            // Treat setpoints within half the 0.1°C control resolution as unchanged, so
            // float-noise between the stored setpoint and a recomputed value doesn't
            // trigger a redundant write (and log line) every cycle.
            auto differs = [](float current, float target) { return std::fabs(current - target) >= 0.05f; };

            if (status.has_independent_zone_temps())
            {
                if (zone == OptimizerZone::ZONE_1) {
                    if (status.is_auto_adaptive_heating(esphome::ecodan::Zone::ZONE_1) && differs(status.Zone1FlowTemperatureSetPoint, flow))
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Z1 Heat Flow -> %.1f°C (%.1f°C)", flow, status.Zone1FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_1);
                        return true;
                    }
                    else if (status.is_auto_adaptive_cooling(esphome::ecodan::Zone::ZONE_1) && differs(status.Zone1FlowTemperatureSetPoint, flow))
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Z1 Cool Flow -> %.1f°C (%.1f°C)", flow, status.Zone1FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_1);
                        return true;
                    }

                } else if (zone == OptimizerZone::ZONE_2) {
                    if (status.is_auto_adaptive_heating(esphome::ecodan::Zone::ZONE_2) && differs(status.Zone2FlowTemperatureSetPoint, flow))
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Z2 Heat Flow -> %.1f°C (%.1f°C)", flow, status.Zone2FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_2);
                        return true;
                    }
                    else if (status.is_auto_adaptive_cooling(esphome::ecodan::Zone::ZONE_2) && differs(status.Zone2FlowTemperatureSetPoint, flow))
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Z2 Cool Flow -> %.1f°C (%.1f°C)", flow, status.Zone2FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_2);
                       return true;
                    }
                }
            }
            else
            {
                if (differs(status.Zone1FlowTemperatureSetPoint, flow)) {
                    if (status.HeatingCoolingMode == esphome::ecodan::Status::HpMode::HEAT_FLOW_TEMP)
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Dependent Heat Flow -> %.1f°C (%.1f°C)", flow, status.Zone1FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_1);
                        return true;
                    }
                    else if (status.HeatingCoolingMode == esphome::ecodan::Status::HpMode::COOL_FLOW_TEMP)
                    {
                        ESP_LOGD(OPTIMIZER_TAG, "CMD: Set Dependent Cool Flow -> %.1f°C (%.1f°C)", flow, status.Zone1FlowTemperatureSetPoint);
                        this->state_.ecodan_instance->set_flow_target_temperature(flow, esphome::ecodan::Zone::ZONE_1);
                        return true;
                    }
                }
            }
            return false;
        }
        
        void Optimizer::on_compressor_stop()
        {
            auto &status = this->state_.ecodan_instance->get_status();

            if (this->state_.lockout_duration->active_index().value_or(0) == 0)
            {
                this->compressor_start_time_ = 0;
                return;
            }

            if (this->compressor_start_time_ == 0 || this->state_.status_short_cycle_lockout->state || status.DefrostActive)
                return;

            uint32_t min_duration_ms = this->state_.minimum_compressor_on_time->state * 60000UL;
            if (min_duration_ms == 0)
                return;

            if (status.is_heating(esphome::ecodan::Zone::ZONE_1) || status.is_cooling(esphome::ecodan::Zone::ZONE_1) || status.is_heating(esphome::ecodan::Zone::ZONE_2) || status.is_cooling(esphome::ecodan::Zone::ZONE_2))
            {

                uint32_t run_duration_ms = millis() - this->compressor_start_time_;
                if (run_duration_ms < min_duration_ms)
                {
                    this->compressor_start_time_ = 0;
                    this->state_.status_short_cycle_lockout->publish_state(true);
                    this->start_lockout();
                }
            }
        }
        
        void Optimizer::on_operation_mode_change(uint8_t new_mode, uint8_t previous_mode)
        {
            if (new_mode == previous_mode) 
                return;

            auto heating_mode = static_cast<uint8_t>(esphome::ecodan::Status::OperationMode::HEAT_ON);
            auto dhw_mode     = static_cast<uint8_t>(esphome::ecodan::Status::OperationMode::DHW_ON);
            auto legionella_mode = static_cast<uint8_t>(esphome::ecodan::Status::OperationMode::LEGIONELLA_PREVENTION);
            auto &status = this->state_.ecodan_instance->get_status();

            if (new_mode != heating_mode && (this->z2_soft_start_pending_ || this->z2_soft_start_active_))
                this->reset_z2_soft_start_("operation left heating");
            if (new_mode != heating_mode && (this->z2_target_delta_soft_start_pending_ || this->z2_target_delta_soft_start_active_))
                this->reset_z2_target_delta_soft_start_("operation left heating");
            if (new_mode != heating_mode) this->cancel_return_guided_startup_("operation left heating / DHW");

            bool entering_dhw = (new_mode == dhw_mode || new_mode == legionella_mode)
                             && (previous_mode != dhw_mode && previous_mode != legionella_mode);

            // Legionella DHW setpoint automation: save/raise the DHW setpoint when a
            // Legionella Prevention run starts, restore the saved value when it ends.
            bool entering_legionella = (new_mode == legionella_mode) && (previous_mode != legionella_mode);
            bool leaving_legionella  = (previous_mode == legionella_mode) && (new_mode != legionella_mode);
            if (entering_legionella || leaving_legionella)
                this->handle_legionella_transition_(entering_legionella);

            // Only save heating setpoints when coming from heating — this is the signal
            // on_feed_temp_change uses to know whether DHW flow-temp following is needed.
            if (entering_dhw) {
                if (previous_mode == heating_mode) {
                    this->dhw_old_z1_setpoint_ = status.Zone1FlowTemperatureSetPoint;
                    this->dhw_old_z2_setpoint_ = status.Zone2FlowTemperatureSetPoint;

                    ESP_LOGD(OPTIMIZER_TAG, "Heating → DHW/Legionella. Saved heating setpoints: Z1=%.1f, Z2=%.1f", 
                        this->dhw_old_z1_setpoint_, this->dhw_old_z2_setpoint_);
                } else {
                    // always clear setpoint
                    this->dhw_old_z1_setpoint_ = NAN;
                    this->dhw_old_z2_setpoint_ = NAN;
                }   
            }

            if (new_mode == heating_mode && previous_mode != heating_mode && this->state_.auto_adaptive_control_enabled->state) {
                ESP_LOGD(OPTIMIZER_TAG, "Operation Mode Changed to heating: %d -> %d", previous_mode, new_mode);
                this->run_auto_adaptive_loop();
            }
        }

        void Optimizer::handle_legionella_transition_(bool entering)
        {
            if (this->state_.legionella_dhw_automation_enabled == nullptr
                || !this->state_.legionella_dhw_automation_enabled->state)
                return;
            if (this->state_.dhw_climate == nullptr)
                return;

            float current = this->state_.dhw_climate->target_temperature;
            if (std::isnan(current))
                return;

            auto *saved_global = this->state_.legionella_saved_dhw_setpoint;

            if (entering) {
                float legionella_sp = (this->state_.legionella_dhw_setpoint != nullptr
                                       && this->state_.legionella_dhw_setpoint->has_state())
                                          ? this->state_.legionella_dhw_setpoint->state
                                          : 56.0f;

                if (current < legionella_sp - 0.05f) {
                    if (saved_global != nullptr)
                        saved_global->value() = current;

                    auto call = this->state_.dhw_climate->make_call();
                    call.set_target_temperature(legionella_sp);
                    call.perform();

                    ESP_LOGI(OPTIMIZER_TAG, "Legionella start: DHW setpoint %.1f -> %.1f (stored %.1f)",
                             current, legionella_sp,
                             saved_global != nullptr ? saved_global->value() : NAN);
                }
            } else {
                float saved_sp = (saved_global != nullptr) ? saved_global->value() : -1.0f;

                if (saved_sp > 0.0f && current > saved_sp + 0.05f) {
                    auto call = this->state_.dhw_climate->make_call();
                    call.set_target_temperature(saved_sp);
                    call.perform();

                    ESP_LOGI(OPTIMIZER_TAG, "Legionella end: restoring DHW setpoint to %.1f", saved_sp);
                }

                // clear the stored value for the next run
                if (saved_global != nullptr)
                    saved_global->value() = -1.0f;
            }
        }

        void Optimizer::on_defrost_state_change(bool x, bool x_previous) 
        { 
            // New defrost, store outside temp
            if (!x_previous && x)
            {
                this->cancel_return_guided_startup_("defrost started");
                if (this->z2_soft_start_pending_ || this->z2_soft_start_active_)
                    this->reset_z2_soft_start_("defrost started");
                if (this->z2_target_delta_soft_start_pending_ || this->z2_target_delta_soft_start_active_)
                    this->reset_z2_target_delta_soft_start_("defrost started");
                auto &status = this->state_.ecodan_instance->get_status();
                DefrostState state_before_defrost{};

                if (!std::isnan(status.OutsideTemperature))
                    state_before_defrost.locked_outside_temp_ = status.OutsideTemperature;

                if (!std::isnan(status.HpReturnTemperature))
                    state_before_defrost.locked_return_temp_ = status.HpReturnTemperature;

                if (!std::isnan(status.Z1ReturnTemperature))
                    state_before_defrost.locked_return_temp_z1_ = status.Z1ReturnTemperature;

                if (!std::isnan(status.Z2ReturnTemperature))
                    state_before_defrost.locked_return_temp_z2_ = status.Z2ReturnTemperature;

                this->state_before_defrost_ = state_before_defrost;
                ESP_LOGD(OPTIMIZER_TAG, "Defrost started. Locking states: outside temp: %.1f, hp return: %.1f, z1 return: %.1f, z2 return: %.1f for adaptive calculations.", 
                    state_before_defrost.locked_outside_temp_, state_before_defrost.locked_return_temp_, state_before_defrost.locked_return_temp_z1_, state_before_defrost.locked_return_temp_z2_);
                
            }
            // Defrost STOP
            else if (x_previous && !x)
            {
                this->last_defrost_time_ = millis();
                ESP_LOGD(OPTIMIZER_TAG, "Defrost stop: triggering auto-adaptive loop (Also updating last_defrost timestamp).");
                this->run_auto_adaptive_loop();
            }
        }

        void Optimizer::on_compressor_state_change(bool x, bool x_previous, bool initial_observation,
                                                  bool initial_startup_observation)
        {
            auto &startup_status = this->state_.ecodan_instance->get_status();
            const bool startup_enabled = this->state_.z2_return_guided_startup != nullptr
                                         && this->state_.z2_return_guided_startup->state;
            const bool startup_eligible = startup_enabled && this->return_startup_ineligible_reason_(startup_status) == nullptr;
            if (!x) this->cancel_return_guided_startup_("compressor stopped");
            const bool startup_started = this->z2_return_startup_.compressor_event(
                millis(), x, x_previous, initial_observation || initial_startup_observation,
                startup_enabled, startup_eligible);
            if (startup_started) {
                this->return_startup_service_ms_ = millis();
                this->reset_z2_soft_start_("Return-Guided Startup has priority for this cycle");
                this->reset_z2_target_delta_soft_start_("Return-Guided Startup has priority for this cycle");
                this->clear_predictive_boost_(OptimizerZone::ZONE_2, false);
                ESP_LOGI(OPTIMIZER_TAG, "[Z2 STARTUP] Return-guided startup activated after genuine compressor start; both legacy startup ramps suspended for this cycle. Buffer and compressor-stop guards remain in charge.");
                this->publish_return_guided_startup_();
            } else if (startup_enabled && x && !x_previous) {
                ESP_LOGI(OPTIMIZER_TAG, "[Z2 STARTUP] Not activated: %s.",
                         (initial_observation || initial_startup_observation) ? "compressor already running at ESP startup"
                         : this->return_startup_ineligible_reason_(startup_status) != nullptr
                           ? this->return_startup_ineligible_reason_(startup_status) : "no eligible start");
            }
            if (x_previous && !x) {
                this->reset_z2_soft_start_("compressor stopped");
                this->reset_z2_target_delta_soft_start_("compressor stopped");
            }
            if (millis() < 60000) {
                if (startup_started) this->run_auto_adaptive_loop(true);
                return;
            }

            if (x_previous && !x)
            {
                ESP_LOGI(OPTIMIZER_CYCLE_TAG, "Compressor STOP detected");
                this->on_compressor_stop();
            }
            else if (!x_previous && x)
            {
                auto &status = this->state_.ecodan_instance->get_status();
                if (!status.DefrostActive)
                {
                    ESP_LOGI(OPTIMIZER_CYCLE_TAG, initial_observation
                             ? "Compressor already running at ESP startup; target-delta ramp is not initialized mid-cycle."
                             : "Compressor START detected");
                    this->compressor_start_time_ = millis();
                    if (!this->z2_return_startup_.claimed_cycle()
                        && this->state_.z2_soft_start_enabled != nullptr
                        && this->state_.z2_soft_start_enabled->state
                        && status.Operation == esphome::ecodan::Status::OperationMode::HEAT_ON
                        && status.has_2zones()
                        && status.is_auto_adaptive_heating(esphome::ecodan::Zone::ZONE_2)
                        && status.is_heating_active(esphome::ecodan::Zone::ZONE_2)) {
                        this->z2_soft_start_pending_ = true;
                        this->z2_soft_start_active_ = false;
                        this->z2_soft_start_start_ms_ = millis();
                        this->z2_soft_start_initial_flow_ = NAN;
                        this->z2_soft_start_last_step_ = -1;
                        ESP_LOGI(OPTIMIZER_TAG, "Z2 soft-start armed after compressor start; waiting for first valid AA flow calculation.");
                    }
                    if (!this->z2_return_startup_.claimed_cycle() && !initial_observation
                        && this->aa_enabled()
                        && this->state_.z2_target_delta_soft_start_enabled != nullptr
                        && this->state_.z2_target_delta_soft_start_enabled->state
                        && status.Operation == esphome::ecodan::Status::OperationMode::HEAT_ON
                        && status.has_2zones()
                        && status.is_auto_adaptive_heating(esphome::ecodan::Zone::ZONE_2)
                        && status.is_heating_active(esphome::ecodan::Zone::ZONE_2)
                        && this->state_.heating_system_type != nullptr
                        && this->state_.heating_system_type->active_index().value_or(0) <= 1) {
                        this->z2_target_delta_soft_start_pending_ = true;
                        this->z2_target_delta_soft_start_active_ = false;
                        this->z2_target_delta_soft_start_reached_raw_ = false;
                        this->z2_target_delta_last_step_ = -1;
                        this->z2_target_delta_initial_raw_ = NAN;
                        this->z2_target_delta_max_raw_seen_ = NAN;
                        ESP_LOGI(OPTIMIZER_TAG, "Z2 target-delta soft-start armed after compressor start; waiting for first valid normal AA calculation.");
                    } else if (initial_observation && x
                               && this->state_.z2_target_delta_soft_start_enabled != nullptr
                               && this->state_.z2_target_delta_soft_start_enabled->state) {
                        ESP_LOGW(OPTIMIZER_TAG, "Compressor was already running at ESP startup; target-delta ramp will not restart mid-cycle. It will arm at the next genuine compressor start.");
                    }
                }
                if (this->state_.auto_adaptive_control_enabled->state)
                {
                    ESP_LOGD(OPTIMIZER_TAG, "Compressor start: triggering auto-adaptive loop.");
                    this->run_auto_adaptive_loop();
                }
            }
        }

    } // namespace optimizer
} // namespace esphome
