#include "optimizer.h"

namespace esphome { namespace optimizer {
void Optimizer::register_guard_telemetry_() {
    auto bind=[this](sensor::Sensor *source, GuardTelemetry::Field field) {
        if (source==nullptr) return;
        source->add_on_state_callback([this,field](float value) {
            this->guard_telemetry_.observe(field,millis(),value);
        });
    };
    bind(this->state_.compressor_frequency,GuardTelemetry::FREQUENCY);
    bind(this->state_.flow_rate,GuardTelemetry::FLOW_RATE);
    bind(this->state_.hp_feed_temp,GuardTelemetry::HP_FEED);
    bind(this->state_.hp_return_temp,GuardTelemetry::HP_RETURN);
    bind(this->state_.z2_feed_temp,GuardTelemetry::Z2_FEED);
    bind(this->state_.z2_return_temp,GuardTelemetry::Z2_RETURN);
}

float Optimizer::apply_z2_guard_recovery_(float legacy, bool aa_source, float normal_aa, float delta) {
    const uint32_t now=millis();
    if (aa_source) {
        this->z2_normal_aa_before_min_=normal_aa;
        this->z2_guard_delta_=delta;
        this->z2_normal_aa_ms_=now;
        this->z2_normal_aa_observed_=std::isfinite(normal_aa) && std::isfinite(delta);
    }
    if (this->state_.z2_guard_recovery_enabled==nullptr || !this->state_.z2_guard_recovery_enabled->state) {
        this->z2_guard_recovery_.reset();
        if (aa_source && millis()-this->z2_guard_log_ms_>=30000UL) {
            const auto bounds=this->get_flow_limits(OptimizerZone::ZONE_2);
            const float normal=std::clamp(normal_aa,bounds.min,bounds.max);
            this->z2_guard_log_ms_=millis();
            ESP_LOGD(OPTIMIZER_TAG,"[Z2 GUARD] Normal AA=%.2f C guard target=%.2f C uplift=%.2f K frequency=%u Hz recovery permitted=NO (disabled).",
                normal,legacy,std::max(0.0f,legacy-normal),static_cast<unsigned>(this->state_.ecodan_instance->get_status().CompressorFrequency));
        }
        return legacy; // Exact original target path when optional recovery is OFF.
    }
    auto &status=this->state_.ecodan_instance->get_status();
    const auto limits=this->get_flow_limits(OptimizerZone::ZONE_2);
    const bool startup=this->z2_return_startup_.owns_flow() || this->z2_return_startup_.waiting_for_aa()
        || this->z2_soft_start_pending_ || this->z2_soft_start_active_
        || this->z2_target_delta_soft_start_pending_ || this->z2_target_delta_soft_start_active_;
    const bool eligible=!startup && this->confirmed_compressor_run_
        && now-this->confirmed_compressor_start_ms_>=1200000UL
        && this->return_startup_ineligible_reason_(status)==nullptr
        && this->state_.predictive_short_cycle_control_enabled!=nullptr
        && this->state_.predictive_short_cycle_control_enabled->state
        && this->z2_normal_aa_observed_ && now-this->z2_normal_aa_ms_<=360000UL
        && this->guard_telemetry_.fresh(now)
        && std::isfinite(limits.min) && std::isfinite(limits.max)
        && limits.max>=20.0f && limits.min<=limits.max;
    if (!eligible) {
        this->z2_guard_recovery_.reset();
        if (now-this->z2_guard_log_ms_>=30000UL) {
            this->z2_guard_log_ms_=now;
            ESP_LOGD(OPTIMIZER_TAG,"[Z2 GUARD] Recovery permitted=NO; startup/run age/demand/sensor/AA freshness or existing predictive guard gate; legacy target=%.2f C.",legacy);
        }
        return legacy;
    }
    const float normal=std::clamp(this->z2_normal_aa_before_min_,limits.min,limits.max);
    // Ask the existing AA guard for its CURRENT strict floor; never duplicate or
    // weaken its Buffer tolerance, feed-minus-1 heuristic, min or max rules.
    const float required=aa_source ? legacy : this->apply_flow_limits(OptimizerZone::ZONE_2,
        this->z2_normal_aa_before_min_,this->z2_guard_delta_,status.Z2ReturnTemperature);
    const float live=this->get_flow_setpoint(OptimizerZone::ZONE_2);
    const auto result=this->z2_guard_recovery_.update(now,true,normal,required,legacy,live,this->guard_telemetry_);
    const float target=std::clamp(result.target,limits.min,limits.max);
    if (result.step) {
        ESP_LOGI(OPTIMIZER_TAG,"[Z2 GUARD] Recovery step: previous=%.2f C new=%.2f C normal AA=%.2f C strict guard floor=%.2f C.",live,target,normal,required);
        this->clear_predictive_boost_(OptimizerZone::ZONE_2,false);
    }
    if (result.step || now-this->z2_guard_log_ms_>=30000UL) {
        this->z2_guard_log_ms_=now;
        ESP_LOGD(OPTIMIZER_TAG,"[Z2 GUARD] Normal AA=%.2f C guard target=%.2f C uplift=%.2f K frequency=%u Hz recovery permitted=%s reason=%s.",
            normal,target,std::max(0.0f,target-normal),static_cast<unsigned>(status.CompressorFrequency),
            result.permitted ? "YES" : "NO",result.reason);
    }
    return target;
}
} }
