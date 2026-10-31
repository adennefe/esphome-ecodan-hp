> Packet-observed compressor initialization and optional conservative guard recovery
> were added in 2026-10-31.01. See [the follow-up review](z2-startup-guard-2026-10-31.md).

# Current behavior: Z2 minimum or maximum duration

This source revision supersedes the timed startup and handover described in the
historical report below. Review revision 2026-10-30.03 corrects a hold-phase
predictive-guard gap found in version 2026-10-30.02. Hardware behavior is untested.
Firmware build 2026-10-30.03 succeeded (ESPHome 2026.9.1 / ESP-IDF 5.5.5,
config_hash=0x3b03dc1a, 2026-10-30 20:45:39 +0100).
184 host assertions passed (91 state machine, 31 flow-limit integration,
47 production eligibility/service/predictive integration, 15 production AA-loop
and heating-write integration). The latter execute the actual production methods
against controlled device fixtures; they do not emulate the physical FTC.

* A genuine eligible compressor start arms the function.
* Return offset: 0-3 K in 0.1 K steps, default 1 K, restored after reboot.
  At 0 K the request follows the measured return without an added offset; this
  does not guarantee positive heat input. Guards and the 20 C floor still apply.
* Startup requests min(primary WP return + offset, normal AA before minimum,
  configured Z2 minimum), retaining the 20 C technical floor, maximum and guards.
* Startup ends when this request, rounded to 0.1 C, reaches the configured Z2
  minimum. Guard-induced higher temperatures alone do not end startup.
* Startup also ends at the configured maximum duration (default 15 min, 5-60 min
  in 5 min steps, restored after reboot). There is no 0.5 K/min handover.
  At timeout the last startup request is retained until the next regular AA cycle.
* After reaching the minimum, timeout or manual switch-off, additional AA
  calculations defer until the next regular AA cycle. The existing predictive
  compressor-stop guard (if enabled) continues during this hold, using its normal
  protective 0.5 K steps and maximum. It does not restore the ordinary Z2 minimum
  early. Invalid prerequisites cancel the hold. The regular AA cycle discards a
  stale protective boost base before taking over; a higher AA request can jump.
* If return+offset or the normal AA cap stays below the minimum, the timer ends
  startup. Stop, invalid data and existing exclusions still
  cancel it. The old two soft starts stay suppressed for the accepted cycle.

Example: minimum 24 C, offset 1 K, normal AA 27 C: WP return 20/21/22/23 C
requests 21/22/23/24 C. At 24 C the next regular AA cycle takes over.

---

# Historical report for the previous, built firmware

# Z2 Return-Guided Startup

Optional experimental startup control for the Z2 fixed-flow heating request. The
switch defaults to OFF. This feature changes the requested Z2 flow temperature;
it does not set compressor frequency or issue compressor start/stop commands.

## Analysis of the existing controller

* `components/optimizer/optimizer.cpp` attaches to the existing `status_compressor`
  callback. `events.cpp::on_compressor_state_change()` handles real transitions.
  The new startup distinguishes the first observation after ESP boot from a
  subsequent OFF-to-ON transition. Enabling the switch mid-cycle does not arm it.
* `components/ecodan/response.cpp`, `TEMPERATURE_STATE_A` (0x0C), decodes
  `status.HpReturnTemperature` and publishes `hp_return_temp`, named **Return Temp**
  in the English labels. This is the primary WP return measurement. Neither
  `BoilerReturnTemperature` nor the mixed Z2 return is used in the startup formula.
* `process_adaptive_zone_()` retains the ordinary cold factor, room error, bias,
  profile and smart boost calculations. `calculate_heating_flow_()` normally adds
  the resulting target delta to the zone return (or uses the profile base delta
  when the room target has been exceeded). The startup cap uses this normal flow
  **before** its configured minimum, so that the minimum cannot inflate the cap.
* Existing order: target-delta soft start -> flow calculation -> flow soft start's
  temporary minimum -> buffer guard -> rounding -> compressor-stop step limiter
  -> zone min/max -> `events.cpp::set_flow_temp()`. The latter suppresses changes
  below 0.05 C and sends the result through
  `ecodan/commands.cpp::set_flow_target_temperature()`.
* The Z2 command writes the Z2 fixed-flow setpoint field of BASIC_SETTINGS, encoded
  as temperature times 100 by `proto.h::set_float16()`. There is no protocol-side
  24 C clamp in this code; 21.5 C is representable as 2150. Read-back and the
  physical plant response still require a hardware test.

Mitsubishi's EHSD-D service documentation lists a 20–60 C heating flow range.
The new exception therefore retains a 20 C lower bound and the user's configured
Z2 maximum; it never forces requests below 20 C. Actual FTC configuration and
model revision can impose additional restrictions.

Primary sources:

* [EHSD-D Hydrobox service manual OCH712, technical specifications](https://mitsubishielectric.pt/documents/20124/44128/ecodansplitdoc6.pdf/91f02cc9-8899-ab86-f365-929d885b8e1c?t=1625052951639)
* [Mitsubishi commissioning settings showing Zone2 heating flow range](https://library.mitsubishielectric.co.uk/pdf/download_full/4254)

The software controls a **Z2** request. The code and protocol do not establish how
the FTC selects its primary heat-production target for this particular buffer
and mixer installation. Z1 demand, FTC configuration and the mixer can dominate
that selection. This is an experimental way to compare Z2 requests, not proof
that the first 30-to-70 Hz ramp will change. If Z2 read-back follows the request
but the primary target/frequency does not, do not bypass the guards or manipulate
Z1 to force an effect. Compare the measurements and check the FTC/hydraulic
configuration first.

## Entities

The existing ESPHome web entity list and Home Assistant expose these controls.
They are not additional controls in the separate Asgard dashboard cards.

| Entity name (Auto-Adaptive prefix) | Initial value | Allowed settings |
|---|---|---|
| Z2 Return-Guided Startup | OFF | ON/OFF; restored after reboot |
| Z2 Startup Return Offset | 1.0 K | 0.5–3.0 K, step 0.5 K; restored |
| Z2 Startup Duration | 15 min | 5–30 min, step 5 min; restored |
| Z2 Startup Active | OFF | Diagnostic binary sensor, true only in the active phase |

State-machine progress is deliberately not restored after reboot. A running
compressor discovered on boot does not start a new startup phase. The next real
OFF-to-ON transition is required.

## Operation and priorities

A genuine compressor start can arm the mode only when AA is enabled, Z2 is
requesting fixed-flow heating, separate Z2 control is available, the required
measurements are valid, and there is no lockout, ODIN takeover, DHW follow window,
defrost or known defrost recovery. The new mode does not depend on the shared
heating-system profile being UFH, so Z1 can remain configured for radiators.
The existing AA writer still requires Z1 fixed-flow mode; the new mode refuses
to arm if that prerequisite is missing and does not change Z1's mode itself.

The primary return must be finite, within 0–80 C and received within the last 120 seconds. The
standard 0x0C request already supplies it; no extra protocol polling is added.

During the active phase:

```
normal_aa_target = normal Z2 flow calculation before configured minimum
startup_target = min(primary_wp_return + offset, normal_aa_target)
startup_target = clamp(startup_target, 20 C, configured Z2 maximum)
```

If the normal AA calculation itself is below 20 C, the new mode cancels rather
than raising it through a conflicting startup formula. Invalid inputs cancel
startup and return to the ordinary calculation/limits. Non-finite flow commands
are rejected for a claimed Z2 startup cycle at the existing write boundary.

Only the configured Z2 **minimum** can be relaxed during startup and its timed
handover. Z1 limits, maxima and Mitsubishi's internal protections are unchanged.
The existing buffer guard uses the original AA delta plus the existing Z2 guard
tolerance and the original zone-return measurement. It can raise the result to
the measured Z2 feed. The existing compressor-stop limiter can also raise a
request that is too far below feed. Their priority is intentionally above the
startup ceiling. A startup request of 21.5 C can therefore become 24 C after a
guard. Both values and the reason are logged.

The separate predictive Z2 writer would otherwise repeatedly restore the normal
minimum. While startup/handover owns the request, it delegates to the AA write
path, which applies the same `enforce_step_limit()` protection. Its private boost
is cleared without restoring an obsolete setpoint. This is one shared guarded
writer, not removal of Mitsubishi safety controls. The predictive Z1 path and
the short-cycle lockout retain their existing behavior.

Both legacy Z2 startup ramps are cancelled when this mode accepts a start and
stay suppressed for that compressor cycle. Their switches and saved settings
are left intact. They do not restart midway through handover or after cancellation.
If the new mode never accepted the cycle (including when OFF), their normal
behavior is retained.

The normal AA loop is reused. Additional evaluations occur at most every 30 s
during startup/handover; unchanged targets do not create extra commands. These
additional evaluations compute/write only Z2 and preserve the shared smart-boost
history, so they do not advance Z1 control. Regular AA calls retain their normal
schedule and calculations.

At duration expiry or when switched OFF, active startup ends and the diagnostic
binary sensor turns off. The handover starts at the last value **after** the
guards and raises its ceiling by 0.5 K per elapsed minute until the current
normal AA target, including its normal minimum, is reached. It uses time rather
than loop count. Guards and max remain in charge throughout. A new compressor
start is required to arm another startup.

Compressor stop, loss of Z2 heating demand, invalid/stale sensors, ODIN takeover,
lockout, DHW, cooling or defrost cancel startup/handover immediately. The ordinary
control path then owns the request; it does not keep using a sensor-based ramp
whose prerequisites have disappeared. That fallback can restore a higher normal
minimum immediately. Known defrost recovery is excluded for the profile's
recovery duration, at least 15 minutes, irrespective of whether the optional
defrost-risk setting is ON.

## Examples before the guards

Assume normal AA is 27 C, normal Z2 minimum 24 C and offset 1 K:

| Primary WP return | Startup request |
|---:|---:|
| 20.5 C | 21.5 C |
| 21.0 C | 22.0 C |
| 22.0 C | 23.0 C |
| 23.0 C | 24.0 C |
| 24.0 C | 25.0 C |

If normal AA before the minimum is 23 C and return+offset is 25 C, startup asks
for 23 C. If a guard holds this at 24.5 C, the applied request is 24.5 C and the
log makes that override explicit.

If startup ends with an applied request of 21.5 C while the normal request is
24 C, handover ceilings are 21.5, 22.0, 22.5, 23.0, 23.5 and 24.0 C at 0–5 min.
An intervening guard is allowed to raise those values.

## Validation and first comparison

Host tests execute the production C++ state machine, not a Python reimplementation:

```
python tests/run_return_guided_startup.py --cxx /path/to/g++
python tests/run_return_guided_startup.py --zig /path/to/zig
```

They exercise OFF and boot behavior, genuine starts, live return changes, AA/max
caps, the 20 C floor, invalid inputs, cancellation eligibility (demand/defrost/
recovery/DHW/lockout/ODIN), cycle ownership, duration, switch-off, handover,
repeated calls, timer wrap and post-guard feedback. The eligibility scenarios
simulate the status decision; they do not emulate Mitsubishi hardware. The
ESPHome build checks the integration, entity references and firmware compilation.

Validation on 2026-10-30: **68 state-machine checks and 30 guard integration checks
passed**. The latter compile the actual `enforce_step_limit()` and
`apply_flow_limits()` bodies from `utility.cpp`, including guard tolerance,
ordinary/temporary minimums, 0.1 C rounding, maximum priority, Z1 isolation and 20 repeated
startup/guard evaluations. Both test executables exited successfully.

The final configuration validation and ESP32-S3 firmware build succeeded with
ESPHome 2026.9.1 / ESP-IDF 5.5.5. The local package identifies itself as
`asgard-z2-en-2026-10-30.01-return-guided-startup-1`; OTA and factory images were
generated. No hardware behavior or 30-to-70 Hz reduction has been verified.

For a first controlled comparison in Home Assistant:

* Z2 Return-Guided Startup: ON **before** the next genuine compressor start.
* Z2 Startup Return Offset: 1.0 K; Z2 Startup Duration: 15 min.
* Leave the normal Z2 minimum at 24 C and the existing safe maximum unchanged.
* Keep guard tolerance and other heating settings constant between comparison runs.
* The old soft-start switches may remain configured; this mode suppresses them
  for an accepted cycle. Turning them OFF for the comparison makes the settings
  easier to inspect.

Record primary Return Temp, primary Flow Temp, Z2 feed/return, requested and
read-back Z2 setpoint, compressor frequency, Z1 request, multizone status and
`[Z2 STARTUP]`/guard logs. Prefer comparison runs where Z1 naturally has no heat
demand. Do not assume a requested value was accepted until its read-back changes.
No hardware test, flashing or OTA is performed as part of development.

## Changed files

* `components/optimizer/return_guided_startup.h`: timed startup/handover state machine.
* `components/optimizer/return_guided_startup.cpp`: status validation, diagnostics,
  sensor freshness and periodic Z2-only service.
* `components/optimizer/auto_adaptive.cpp`: normal AA cap, temporary minimum,
  legacy-ramp priority, post-guard feedback, logging and isolation of extra Z2 calls.
* `components/optimizer/events.cpp`: start/stop/defrost/DHW lifecycle and scoped
  rejection of non-finite Z2 startup-cycle requests.
* `components/optimizer/optimizer.cpp`, `optimizer.h`, `optimizer_state.h`:
  callback integration, declarations and entity connections.
* `components/optimizer/prevention.cpp`: shared guarded writer while startup owns Z2.
* `confs/auto-adaptive.yaml`: switch, numbers, diagnostic sensor and service interval.
* `confs/ecodan-labels-*.yaml`: labels for all existing language configurations.
* `tests/return_guided_startup_test.cpp`, `startup_guard_integration_test.cpp`,
  `run_return_guided_startup.py`: executable host regression tests.
* `docs/z2-return-guided-startup.md`: analysis, priorities, examples and test procedure.

The local manual-upload project has its own versioned configuration and build
directory. The previous 2026-10-26.01 and 2026-10-26.02 artifacts are preserved.
