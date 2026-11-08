#include "optimizer.h"

namespace esphome { namespace optimizer {

const char *Optimizer::flow_ramp_ineligible_reason_(const ecodan::Status &status) {
    if (const char *reason = this->return_startup_ineligible_reason_(status)) return reason;
    const uint32_t now = millis();
    if (!this->guard_telemetry_.fresh(now)) return "missing, invalid or stale telemetry (>120 s)";
    for (auto field : {GuardTelemetry::HP_FEED, GuardTelemetry::HP_RETURN,
                       GuardTelemetry::Z2_FEED, GuardTelemetry::Z2_RETURN}) {
        const float value = this->guard_telemetry_.values[field];
        if (value < 0 || value > 80) return "water temperature outside 0..80 C";
    }
    if (status.Z2FeedTemperature < 0 || status.Z2FeedTemperature > 80
        || status.Z2ReturnTemperature < 0 || status.Z2ReturnTemperature > 80)
        return "Z2 temperature outside 0..80 C";
    if (this->guard_telemetry_.values[GuardTelemetry::FLOW_RATE] < 10 || status.FlowRate < 10)
        return "flow below 10 l/min";
    const float hz = this->guard_telemetry_.values[GuardTelemetry::FREQUENCY];
    if (hz < 0 || hz > 150) return "invalid compressor frequency";
    const auto limits = this->get_flow_limits(OptimizerZone::ZONE_2);
    if (!std::isfinite(limits.min) || !std::isfinite(limits.max) || limits.max < 24 || limits.min > limits.max)
        return "invalid Z2 flow limits";
    const float room = this->get_room_current_temp(OptimizerZone::ZONE_2);
    const float target = this->get_room_target_temp(OptimizerZone::ZONE_2);
    if (room <= target - 0.2f + 0.0001f) return "room deficit at least 0.2 K";
    return nullptr;
}

bool Optimizer::start_z2_flow_ramp_(bool running, bool previous, bool initial) {
    auto &s = this->state_.ecodan_instance->get_status();
    auto *enable = this->state_.z2_flow_anchored_startup;
    const bool selected = enable != nullptr && enable->state;
    const char *reason = selected ? this->flow_ramp_ineligible_reason_(s) : "disabled";
    const float delta = this->state_.z2_flow_ramp_initial_delta != nullptr
        ? this->state_.z2_flow_ramp_initial_delta->state : 2.0f;
    if (!running) this->cancel_z2_flow_ramp_("compressor stopped");
    const bool started = this->z2_flow_ramp_.compressor_event(millis(), running, previous, initial,
        selected, reason == nullptr, s.Z2ReturnTemperature, delta);
    if (started) {
        this->flow_ramp_service_ms_ = millis();
        // Only observations after this start qualify the five-minute HOLD window.
        this->cancel_return_guided_startup_("Flow-Anchored Startup selected");
        this->reset_z2_soft_start_("Flow-Anchored Startup selected");
        this->reset_z2_target_delta_soft_start_("Flow-Anchored Startup selected");
        this->clear_predictive_boost_(OptimizerZone::ZONE_2, false);
        ESP_LOGI(OPTIMIZER_TAG, "[Z2 FLOW RAMP] Activated at genuine start: saved Z2 return=%.2f C initial delta=%.1f K fixed base=%.2f C. Other startup ramps suspended for entire compressor cycle.",
            this->z2_flow_ramp_.initial_return(), delta, this->z2_flow_ramp_.base());
    } else if (selected && running && !previous) {
        ESP_LOGI(OPTIMIZER_TAG, "[Z2 FLOW RAMP] Not activated: %s.", initial
            ? "initial ON observation; no confirmed OFF->ON" : reason != nullptr ? reason : "invalid start reference/configuration");
    }
    return started;
}

void Optimizer::cancel_z2_flow_ramp_(const char *reason) {
    if (this->z2_flow_ramp_.active()) {
        ESP_LOGI(OPTIMIZER_TAG, "[Z2 FLOW RAMP] Released: %s; elapsed=%.2f min; next owner=normal AA and existing guards.",
            reason, this->z2_flow_ramp_.elapsed_ms(millis()) / 60000.0f);
        this->clear_predictive_boost_(OptimizerZone::ZONE_2, false);
    }
    this->z2_flow_ramp_.cancel(reason);
}

void Optimizer::service_z2_flow_ramp(bool force) {
    if (!this->z2_flow_ramp_.active()) return;
    auto &s = this->state_.ecodan_instance->get_status();
    const char *reason = this->flow_ramp_ineligible_reason_(s);
    const float duration = this->state_.z2_startup_duration->state;
    if (reason == nullptr && !this->state_.z2_flow_anchored_startup->state) reason = "disabled by user";
    if (reason == nullptr && (!std::isfinite(duration) || this->z2_flow_ramp_.elapsed_ms(millis()) >=
        static_cast<uint32_t>(std::clamp(duration, 5.0f, 60.0f) * 60000))) reason = "maximum startup duration reached / invalid duration";
    if (reason != nullptr) {
        this->cancel_z2_flow_ramp_(reason);
        // Immediate return to ordinary min/AA, using the same writer and its hands-off gates.
        this->run_auto_adaptive_loop(true);
        return;
    }
    const bool held = this->z2_flow_ramp_.holding();
    this->z2_flow_ramp_.observe_room(millis(), this->get_room_current_temp(OptimizerZone::ZONE_2),
        this->get_room_target_temp(OptimizerZone::ZONE_2));
    if (force || held != this->z2_flow_ramp_.holding() || millis() - this->flow_ramp_service_ms_ >= 30000UL) {
        this->flow_ramp_service_ms_ = millis();
        this->run_auto_adaptive_loop(true);
    }
}

void Optimizer::log_z2_flow_ramp_(float normal, float before, float after, const char *reason) {
    auto &s = this->state_.ecodan_instance->get_status();
    ESP_LOGD(OPTIMIZER_TAG,
        "[Z2 FLOW RAMP] elapsed=%.2f min frequency=%u Hz WP feed/return=%.2f/%.2f C Z2 feed/return=%.2f/%.2f C saved start return=%.2f C normal AA=%.2f C cap=%.2f C before guards=%.2f C final request after guards=%.2f C HOLD=%s reason=%s. Final request is not FTC read-back confirmation.",
        this->z2_flow_ramp_.elapsed_ms(millis()) / 60000.0f, static_cast<unsigned>(s.CompressorFrequency),
        s.HpFeedTemperature, s.HpReturnTemperature, s.Z2FeedTemperature, s.Z2ReturnTemperature,
        this->z2_flow_ramp_.initial_return(), normal, this->z2_flow_ramp_.cap(), before, after,
        this->z2_flow_ramp_.holding() ? "ON" : "OFF", reason);
}
} }  // namespace esphome::optimizer
