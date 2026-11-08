#include "optimizer.h"

namespace esphome { namespace optimizer {
FlowAnchoredStartup::Buffer Optimizer::flow_ramp_buffer_() const {
    const auto &s=this->state_.ecodan_instance->get_status();
    FlowAnchoredStartup::Buffer buffer;
    buffer.enabled=this->state_.z2_flow_anchored_startup!=nullptr && this->state_.z2_flow_anchored_startup->state
        && this->state_.z2_buffer_guided_mode!=nullptr && this->state_.z2_buffer_guided_mode->state;
    buffer.valid=FlowAnchoredStartup::valid_thw10(s.has_mixing_tank(),s.MixingTankTemperature);
    buffer.temperature=s.MixingTankTemperature;
    buffer.z2_feed=s.Z2FeedTemperature;
    if (this->state_.z2_buffer_ready_margin!=nullptr) buffer.margin=this->state_.z2_buffer_ready_margin->state;
    if (this->state_.z2_buffer_ready_duration!=nullptr) buffer.ready_minutes=this->state_.z2_buffer_ready_duration->state;
    return buffer; // THW10 has no reception timestamp, expiry or value-change requirement.
}

const char *Optimizer::flow_ramp_ineligible_reason_(const ecodan::Status &status) {
    if (const char *reason=this->return_startup_ineligible_reason_(status)) return reason;
    const uint32_t now=millis();
    // Preserve 120 s communications protection for water/flow telemetry. Frequency is diagnostic only here.
    for (auto field : {GuardTelemetry::FLOW_RATE,GuardTelemetry::HP_FEED,GuardTelemetry::HP_RETURN,
                       GuardTelemetry::Z2_FEED,GuardTelemetry::Z2_RETURN}) {
        if (!this->guard_telemetry_.observed[field] || now-this->guard_telemetry_.received[field]>120000UL)
            return "missing, invalid or stale WP/Z2/flow telemetry (>120 s)";
    }
    for (auto field : {GuardTelemetry::HP_FEED,GuardTelemetry::HP_RETURN,GuardTelemetry::Z2_FEED,GuardTelemetry::Z2_RETURN}) {
        const float value=this->guard_telemetry_.values[field];
        if (value<0 || value>80) return "water temperature outside 0..80 C";
    }
    if (status.Z2FeedTemperature<0 || status.Z2FeedTemperature>80 || status.Z2ReturnTemperature<0 || status.Z2ReturnTemperature>80)
        return "Z2 temperature outside 0..80 C";
    if (this->guard_telemetry_.values[GuardTelemetry::FLOW_RATE]<10 || status.FlowRate<10) return "flow below 10 l/min";
    const auto limits=this->get_flow_limits(OptimizerZone::ZONE_2);
    if (!std::isfinite(limits.min)||!std::isfinite(limits.max)||limits.min<20||limits.max<20||limits.min>limits.max)
        return "invalid Z2 flow limits";
    if (this->get_room_target_temp(OptimizerZone::ZONE_2)-this->get_room_current_temp(OptimizerZone::ZONE_2)>=0.5f-0.0001f)
        return "urgent room deficit at least 0.5 K";
    return nullptr;
}

void Optimizer::log_z2_flow_ramp_transition_(FlowAnchoredStartup::Phase previous) {
    if (previous==this->z2_flow_ramp_.phase()) return;
    ESP_LOGI(OPTIMIZER_TAG,"[Z2 FLOW RAMP] Phase %s -> %s; reason=%s; elapsed=%.2f min.",
        FlowAnchoredStartup::phase_name(previous),this->z2_flow_ramp_.phase_name(),this->z2_flow_ramp_.reason(),
        this->z2_flow_ramp_.elapsed_ms(millis())/60000.0f);
}

bool Optimizer::start_z2_flow_ramp_(bool running,bool previous,bool initial) {
    auto &s=this->state_.ecodan_instance->get_status();
    const bool selected=this->state_.z2_flow_anchored_startup!=nullptr && this->state_.z2_flow_anchored_startup->state;
    const char *reason=selected ? this->flow_ramp_ineligible_reason_(s) : "disabled";
    const float delta=this->state_.z2_flow_ramp_initial_delta!=nullptr ? this->state_.z2_flow_ramp_initial_delta->state : 1.0f;
    if (!running && this->z2_flow_ramp_.active()) this->clear_predictive_boost_(OptimizerZone::ZONE_2,false);
    const auto phase=this->z2_flow_ramp_.phase();
    const bool started=this->z2_flow_ramp_.compressor_event(millis(),running,previous,initial,selected,
        reason==nullptr,s.Z2ReturnTemperature,delta);
    this->log_z2_flow_ramp_transition_(phase);
    if (started) {
        this->flow_ramp_service_ms_=millis();
        this->cancel_return_guided_startup_("Flow-Anchored Startup selected");
        this->reset_z2_soft_start_("Flow-Anchored Startup selected");
        this->reset_z2_target_delta_soft_start_("Flow-Anchored Startup selected");
        this->clear_predictive_boost_(OptimizerZone::ZONE_2,false);
        ESP_LOGI(OPTIMIZER_TAG,"[Z2 FLOW RAMP] Activated at genuine start: saved Z2 return=%.2f C initial delta=%.1f K fixed base=%.2f C; buffer mode=%s. Old startup ramps suspended for entire compressor cycle.",
            this->z2_flow_ramp_.initial_return(),delta,this->z2_flow_ramp_.base(),this->flow_ramp_buffer_().enabled ? "ON" : "OFF");
    } else if (selected && running && !previous) {
        ESP_LOGI(OPTIMIZER_TAG,"[Z2 FLOW RAMP] Not activated: %s.",initial ? "initial ON; no confirmed OFF->ON"
            : reason!=nullptr ? reason : "invalid start reference/configuration");
    }
    return started;
}

void Optimizer::cancel_z2_flow_ramp_(const char *reason) {
    const auto phase=this->z2_flow_ramp_.phase();
    if (this->z2_flow_ramp_.active()) this->clear_predictive_boost_(OptimizerZone::ZONE_2,false);
    this->z2_flow_ramp_.cancel(reason);
    this->log_z2_flow_ramp_transition_(phase);
}

void Optimizer::service_z2_flow_ramp(bool force) {
    if (!this->z2_flow_ramp_.active()) return;
    const auto phase=this->z2_flow_ramp_.phase();
    const char *reason=this->flow_ramp_ineligible_reason_(this->state_.ecodan_instance->get_status());
    const float duration=this->state_.z2_startup_duration->state;
    if (reason==nullptr && !this->state_.z2_flow_anchored_startup->state) reason="disabled by user";
    if (reason==nullptr && (!std::isfinite(duration)||this->z2_flow_ramp_.elapsed_ms(millis())>=
        static_cast<uint32_t>(std::clamp(duration,5.0f,60.0f)*60000))) reason="maximum total startup and handover duration reached";
    if (reason!=nullptr) this->cancel_z2_flow_ramp_(reason);
    else {
        this->z2_flow_ramp_.observe_room(millis(),this->get_room_current_temp(OptimizerZone::ZONE_2),this->get_room_target_temp(OptimizerZone::ZONE_2));
        const auto limits=this->get_flow_limits(OptimizerZone::ZONE_2);
        // Observe readiness at 5 s; only a fresh AA calculation may initiate handover.
        this->z2_flow_ramp_.observe_buffer(millis(),this->flow_ramp_buffer_(),limits.min,limits.max,false);
        this->log_z2_flow_ramp_transition_(phase);
    }
    if (force || phase!=this->z2_flow_ramp_.phase() || millis()-this->flow_ramp_service_ms_>=30000UL) {
        this->flow_ramp_service_ms_=millis();
        this->run_auto_adaptive_loop(true); // Same Z2 writer; no extra Z1/relay/history updates.
    }
}

void Optimizer::log_z2_flow_ramp_(float normal,float before,float after,const char *reason) {
    auto &s=this->state_.ecodan_instance->get_status();
    const auto b=this->flow_ramp_buffer_();
    const auto limits=this->get_flow_limits(OptimizerZone::ZONE_2);
    ESP_LOGD(OPTIMIZER_TAG,
        "[Z2 FLOW RAMP] phase=%s elapsed=%.2f min THW10=%.2f C valid=%s buffer mode=%s effective=%s ready threshold=%.2f C condition duration=%.2f min / %.1f min saved Z2 start return=%.2f C cap=%.2f C normal AA before min=%.2f C configured min=%.2f C room/target=%.2f/%.2f C room deficit duration=%.2f min guard override duration=%.2f min WP feed/return=%.2f/%.2f C Z2 feed/return=%.2f/%.2f C frequency=%u Hz (diagnostic only) before guards=%.2f C final request=%.2f C previous requested=%.2f C FTC read-back=%.2f C reason=%s. Request is not read-back confirmation.",
        this->z2_flow_ramp_.phase_name(),this->z2_flow_ramp_.elapsed_ms(millis())/60000.0f,b.temperature,b.valid ? "YES" : "NO",
        b.enabled ? "ON" : "OFF",b.enabled && b.valid ? "THW10" : "time-only fallback",this->z2_flow_ramp_.buffer_threshold(),
        this->z2_flow_ramp_.ready_ms(millis())/60000.0f,b.ready_minutes,this->z2_flow_ramp_.initial_return(),this->z2_flow_ramp_.cap(),normal,limits.min,
        this->get_room_current_temp(OptimizerZone::ZONE_2),this->get_room_target_temp(OptimizerZone::ZONE_2),this->z2_flow_ramp_.room_need_ms(millis())/60000.0f,
        this->z2_flow_ramp_.guard_ms(millis())/60000.0f,s.HpFeedTemperature,s.HpReturnTemperature,s.Z2FeedTemperature,s.Z2ReturnTemperature,
        static_cast<unsigned>(s.CompressorFrequency),before,after,this->z2_flow_ramp_.last_requested(),s.Zone2FlowTemperatureSetPoint,reason);
}
} } // namespace esphome::optimizer
