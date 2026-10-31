#include "optimizer.h"

namespace esphome { namespace optimizer {
void Optimizer::register_compressor_observer_() {
    auto *device = this->state_.ecodan_instance;
    device->add_on_compressor_state_callback([this](bool running) {
        this->observe_compressor_state_(running ? 1.0f : 0.0f);
    });
    // A late subscriber can seed only from a real received packet. The binary
    // sensor's initial false/has_state is explicitly not evidence of an OFF.
    if (device->has_received_compressor_state())
        this->observe_compressor_state_(device->get_status().CompressorOn ? 1.0f : 0.0f);
}

void Optimizer::observe_compressor_state_(float value) {
    if (std::isfinite(value) && this->compressor_observer_.known()
        && (millis()-this->compressor_observed_ms_)>120000UL) {
        // A communications gap can hide a stop/restart. Never infer continuity.
        this->observe_compressor_state_(NAN);
    }
    const auto event = this->compressor_observer_.observe(value);
    if (!event.valid) {
        this->confirmed_compressor_run_ = false;
        this->z2_guard_recovery_.reset();
        this->z2_normal_aa_observed_=false;
        this->cancel_return_guided_startup_("compressor state unknown");
        ESP_LOGW(OPTIMIZER_TAG, "[COMPRESSOR] State unknown; a new confirmed OFF->ON is required.");
        return;
    }
    this->compressor_observed_ms_ = millis();
    if (!event.changed) return;
    this->z2_guard_recovery_.reset();
    this->z2_normal_aa_observed_=false;
    if (event.started()) {
        this->confirmed_compressor_start_ms_ = millis();
        this->confirmed_compressor_run_ = true;
    } else if (event.initial || event.stopped()) {
        this->confirmed_compressor_run_ = false;
    }
    ESP_LOGI(OPTIMIZER_TAG,
        "[COMPRESSOR] Previous=%s current=%s valid transition=%s initialization completed=YES source=received ACTIVE_TIME packet.",
        event.initial ? "UNKNOWN" : (event.previous ? "ON" : "OFF"),
        event.running ? "ON" : "OFF", event.initial ? "NO (first observation)" : "YES");
    this->on_compressor_state_change(event.running, event.previous, event.initial);
}
} }
