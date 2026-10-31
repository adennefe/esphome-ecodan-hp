#include "optimizer.h"

namespace esphome {
namespace optimizer {

const char *Optimizer::return_startup_ineligible_reason_(const ecodan::Status &status) {
    if (!this->aa_enabled()) return "Auto-Adaptive disabled";
    if (this->odin_forwarder_takeover_active()) return "ODIN owns flow control";
    if (!status.CompressorOn) return "compressor stopped";
    if (!this->compressor_observer_.known() || (millis() - this->compressor_observed_ms_) > 120000UL)
        return "compressor observation unknown or stale (>120 s)";
    if (!status.has_2zones() || !status.has_independent_zone_temps()) return "independent Z2 control unavailable";
    if (status.HeatingCoolingMode != ecodan::Status::HpMode::HEAT_FLOW_TEMP
        && status.HeatingCoolingMode != ecodan::Status::HpMode::COOL_FLOW_TEMP)
        return "existing AA write path requires Z1 fixed-flow mode";
    if (status.DefrostActive) return "defrost active";
    const int profile = this->state_.heating_system_type->active_index().value_or(0);
    const uint32_t recovery_ms = std::max(15UL * 60000UL,
        static_cast<unsigned long>(this->get_heating_profile_(profile).defrost_memory_ms));
    if (this->last_defrost_time_ > 0 && (millis() - this->last_defrost_time_) < recovery_ms)
        return "defrost recovery";
    if (status.Operation != ecodan::Status::OperationMode::HEAT_ON) return "operation left heating";
    if (!status.is_auto_adaptive_heating(ecodan::Zone::ZONE_2)
        || !status.is_heating_active(ecodan::Zone::ZONE_2)) return "Z2 heating demand ended";
    if (this->is_post_dhw_window(status)) return "post-DHW follow window";
    if (this->state_.status_short_cycle_lockout != nullptr && this->state_.status_short_cycle_lockout->state)
        return "short-cycle lockout";
    if (!this->wp_return_observed_ || (millis() - this->wp_return_received_ms_) > 120000UL
        || !std::isfinite(status.HpReturnTemperature) || status.HpReturnTemperature < 0.0f
        || status.HpReturnTemperature > 80.0f) return "WP Return Temp invalid, outside 0..80 C or stale (>120 s)";
    if (!std::isfinite(status.Z2FeedTemperature) || !std::isfinite(status.Z2ReturnTemperature)
        || !std::isfinite(status.HpFeedTemperature) || !std::isfinite(status.OutsideTemperature)
        || !std::isfinite(this->get_room_current_temp(OptimizerZone::ZONE_2))
        || !std::isfinite(this->get_room_target_temp(OptimizerZone::ZONE_2))) return "required AA sensor unavailable";
    return nullptr;
}

void Optimizer::publish_return_guided_startup_() {
    auto *sensor = this->state_.z2_startup_active;
    const bool active = this->z2_return_startup_.phase() == ReturnGuidedStartup::Phase::ACTIVE;
    if (sensor != nullptr && (!sensor->has_state() || sensor->state != active)) sensor->publish_state(active);
}

void Optimizer::log_return_startup_finished_(const char *reason) {
    float maximum = this->state_.z2_startup_duration->state;
    if (!std::isfinite(maximum)) maximum = 15.0f;
    maximum = std::clamp(maximum, 5.0f, 60.0f);
    ESP_LOGI(OPTIMIZER_TAG,
        "[Z2 STARTUP] Finished. Reason=%s; elapsed=%.2f min; configured maximum=%.0f min; next owner=normal AA with existing guards.",
        reason, this->z2_return_startup_.elapsed_ms(millis()) / 60000.0f, maximum);
}

void Optimizer::cancel_return_guided_startup_(const char *reason) {
    const bool owned = this->z2_return_startup_.owns_flow() || this->z2_return_startup_.waiting_for_aa();
    if (this->z2_return_startup_.owns_flow()) this->log_return_startup_finished_(reason);
    else if (owned) ESP_LOGI(OPTIMIZER_TAG, "[Z2 STARTUP] Hold ended. Reason=%s.", reason);
    if (owned) this->clear_predictive_boost_(OptimizerZone::ZONE_2, false);
    this->z2_return_startup_.cancel();
    this->publish_return_guided_startup_();
}

void Optimizer::service_return_guided_startup(bool force) {
    if (this->z2_return_startup_.waiting_for_aa()) {
        const char *reason = this->return_startup_ineligible_reason_(this->state_.ecodan_instance->get_status());
        if (reason != nullptr) this->cancel_return_guided_startup_(reason);
        return; // No extra AA calculation during the hold; predictive guard remains active.
    }
    if (this->z2_return_startup_.owns_flow()
        && !this->state_.z2_return_guided_startup->state) {
        this->log_return_startup_finished_("startup disabled");
        this->z2_return_startup_.wait_for_aa();
        this->publish_return_guided_startup_();
        ESP_LOGI(OPTIMIZER_TAG, "[Z2 STARTUP] Disabled; waiting for the next regular AA cycle.");
        return;
    }
    // Reuse the one AA writer, at 30 s only while startup owns the flow.
    if (this->z2_return_startup_.owns_flow() && (force || (millis() - this->return_startup_service_ms_) >= 30000UL)) {
        this->return_startup_service_ms_ = millis();
        this->run_auto_adaptive_loop(true);
    }
}

}  // namespace optimizer
}  // namespace esphome
