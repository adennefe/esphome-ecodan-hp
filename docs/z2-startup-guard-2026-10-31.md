# Z2 Startup and Guard review - 2026-10-31

## Scope and evidence

Base: cdc67cc, firmware 2026-10-30.03. The request supplies log excerpts,
not the full measured run. The excerpts establish requested guard adjustments,
not the cause of primary temperature changes or a guaranteed frequency effect.
The user additionally reports an ESP firmware update at room 19.4 C, target
19.0 C, Asgard hysteresis 0.5 K, followed by actual heatpump shutdown.

## A. Compressor observation: verified initialization defect

`components/ecodan/ecodan.h::register_binarySensor()` publishes a synthetic OFF
when registering an entity. `BinarySensor` deduplicates unchanged states;
ESPHome also disables the initial ordinary callback by default. Consequently a
real OFF packet can leave the optimizer without an OFF callback. The old
optimizer starts its float history at 0, whereas Return-Guided Startup tracked
whether any callback was received. The first ON can therefore produce both
"Compressor START detected" and "already running at ESP startup".

Setting only trigger_on_initial_state or seeding from the entity would be wrong:
the entity's OFF is not a received compressor observation. If the compressor
was running across a firmware update, that would manufacture an OFF-to-ON start.

Fix: `response.cpp` reports received ACTIVE_TIME observations through one packet
callback, including unchanged OFF and ON. `compressor_state.cpp/.h` uses UNKNOWN,
confirmed OFF and confirmed ON. First ON and late subscription while ON never
arm a startup; confirmed OFF followed by ON does. Duplicates update freshness
without restarting runtime. Unknown/invalid observations require a fresh known
OFF-to-ON. Initial observations no longer create legacy START/STOP events. The compressor
entity itself now also remains UNKNOWN until its first actual packet; defaults
of the other status entities are unchanged.

There is no startup delay used as a substitute for a confirmed transition.
The supplied skipped-start log proves that the old initial-observation flag was
set. It does not, by itself, prove whether the compressor physically started
then or was already running across an ESP reboot. The new packet logs separate
these cases. Since startup was reported inactive, the 4 h 51 min run is not
evidence that Return-Guided Startup caused the improved modulation.

## Additional update/room-temperature observation

`confs/asgard-thermostat.yaml` sets heat_deadband=0 and heat_overrun=hysteresis.
The actual ESPHome `heating_required_()` turns on at <=19.0 C, off at >=19.5 C,
and preserves the previous HEATING action in between. Across an ESP reboot that
HEATING action is not restored as an active call. At 19.4 C, a fresh evaluation
can therefore be idle and release the demand relay. This is a plausible update
related shutdown; actual relay/thermostat logs are needed for causal proof.
This task does not change thermostat hysteresis, relay restoration or room targets.

## B. Startup order and termination

Normal AA calculation remains independent. Startup uses the primary Return Temp
(`HpReturnTemperature` from TEMPERATURE_STATE_A), not Boiler Return or Z2 return.
Startup requests min(primary return + offset, normal AA before minimum, Z2 min),
subject to its 20 C technical floor and configured maximum. During startup the
ordinary minimum alone has a local exception. Buffer and compressor-stop guards
remain stronger, so they can raise the final request. This is logged explicitly.

The configured duration remains a MAXIMUM: default 15 min, configurable 5-60 min.
Reaching the calculated minimum target can finish earlier, including at elapsed
zero on a warm start. It is not a measured buffer temperature. No blanket 30 min
change was made. Stop, lost demand, mode changes, defrost, stale/invalid data and
manual disable terminate for their respective reason. Every active termination
logs reason, elapsed minutes and configured maximum. The normal operating target
below configured minimum is not an added early-exit rule; timeout handles that.
Normal AA resumes on its next regular cycle without a separate handover ramp.
Final request/queued command must be distinguished from actual FTC read-back.

## C. Guard: code-proven behavior versus hardware hypothesis

`utility.cpp::apply_flow_limits()` first applies the Buffer delta-T guard:
Z2 feed minus Z2 return > target delta + Z2 tolerance holds at Z2 feed.
The tolerance (5 K here) affects only THIS threshold.
`enforce_step_limit()` then prevents a heating request at least 1 K below the
measured feed by replacing it with feed minus 1 K. With independent zones the
feed is Z2FeedTemperature, not HpFeedTemperature. It does not use compressor
frequency. At Z2 feed 29 C and normal AA 27 C it therefore requests at least 28 C,
even at stable 16 Hz. This is an Asgard stop-avoidance heuristic, not a Mitsubishi
internal safety function or proof that a stop would otherwise happen.

The separate predictive path increases protection by up to 0.5 K per call. It
can discard its private old base without restoring a lower normal request.
Regular AA still recomputes normal AA and the current strict floor every 5 min,
so the code is not a permanently latched positive increment. But when measured
Z2 feed stays high, feed-minus-1 keeps the request high too. Plant feedback can
therefore sustain elevated requests. The provided primary temperature excerpts
do not prove that this feedback caused the temperature rise. The configured
Z2 maximum of 31 C bounds the requested Z2 target, not the measured primary WP
feed temperature. A primary reading of 31-32 C does not by itself establish a
violation of that Z2 request limit.

### Reconstructing the supplied guard excerpts

Under the stated Z2 heating configuration, feed-minus-1 gives:

| Time | Logged pre-guard request | Guard result | Implied Z2 feed | Uplift over logged input |
|---|---:|---:|---:|---:|
| 03:24:29 | 25.30 C | 26.00 C | 27.00 C | +0.70 K |
| 05:39:30 | 27.00 C | 28.00 C | 29.00 C | +1.00 K |
| 06:49:30 | not supplied | 28.50 C | 29.50 C | not determinable |

Implied feed is derived from the actual guard formula, not an independently
recorded primary WP measurement. Guard result is not proof of queued command
or FTC read-back. The excerpts do not align frequency samples to every row;
the code itself has no low-frequency exemption.

## D. Optional conservative Guard Recovery

New switch: `Auto-Adaptive: Z2 Guard Recovery`, RESTORE_DEFAULT_OFF.
It is a postprocessing part of the existing AA/predictive write paths, not a
second thermostat, frequency controller or independent setpoint publisher.
Normal AA and the current guard uplift are recorded separately for diagnosis.

Required before recovery:
- A confirmed packet-observed start and at least 20 minutes continuous runtime.
- AA and the existing Predictive Short-Cycle Control enabled, Z2 demand and
  independent Z2 flow control, no ODIN takeover, DHW, defrost/recovery or lockout.
- No competing Return-Guided, flow-soft-start or target-delta startup phase.
- Normal AA data at most 6 minutes old, compressor data at most 120 s old.
- Fresh (<=120 s) primary/Z2 feed and return, frequency and flow measurements.
- Ten minutes of observations with gaps <=90 s, frequency 16-26 Hz with spread
  <=4 Hz, primary flow >=5 l/min with spread <=2 l/min, each water temperature
  span <=0.5 K, primary/Z2 feed no more than 0.2 K above the window start,
  feed not more than 0.2 K below its corresponding return.
- The previous requested target confirmed in device read-back within 0.1 K.

A permitted reduction is at most 0.5 K per ten minutes. It is bounded BELOW by
the result of the CURRENT original AA guard (Buffer guard, feed-minus-1, min/max)
and bounded ABOVE by the user's max. Instability/new guard need immediately
falls back to the original guard target and restarts observation. Disabling the
switch takes the exact original target path. No Mitsubishi protection or guard
floor is weakened. Protective increase remains available during recovery.

Important limitation: at normal AA 27, Z2 feed 29.5 and strict floor 28.5, this
optimization CANNOT lower the request to 28 or 27 while those measurements hold.
That would require relaxing the existing stop-avoidance threshold without a
verified FTC model. The new option only unwinds a HELD uplift when the current
floor permits it; it may deliberately hold a higher target longer than the old
instant AA return, to make the decay gradual. It does not guarantee better COP
or a lower steady temperature. Low frequency alone never authorizes reduction.

## Test/build status

297 assertions passed in seven host programs:
91 startup lifecycle, 31 production guard integration, 47 production eligibility
and service integration, 15 production AA integration, 21 real packet/observer
regressions, 86 conservative recovery checks, 6 installed ESPHome thermostat
hysteresis/restart checks. Device I/O is simulated; production methods and Ecodan
status predicates are compiled directly. No complete FTC/physical plant model.

Reproduce with Python + installed ESPHome + a C++17 compiler:
`python tests/run_return_guided_startup.py --cxx /path/to/g++`
(or `--zig /path/to/zig`). ESPHome 2026.9.1 is used for thermostat source extraction.

Firmware 2026-10-31.01 build SUCCEEDED with ESPHome 2026.9.1 / ESP-IDF 5.5.5.
config_hash=0xe247d014 build_time_str=2026-10-31 08:51:32 +0100
RAM 45.3% (154971 bytes), Flash 37.2% (1460799 bytes).
No OTA/flash or compressor start/stop commands were issued.
The CRLF style of the two existing Ecodan files is preserved; whitespace check
passes with Git's cr-at-eol option.

## Next hardware comparison

First verify ONLY corrected startup detection. Keep Guard Recovery OFF and the
existing stable settings: offset 1 K, maximum duration 15 min, Z2 min/max 24/31 C,
tolerance 5 K, both old soft starts OFF. Enable Startup BEFORE a genuine next
compressor start; avoid updating in the middle of a run. Do not increase room
or water targets merely to force a test start.

Only after a comparable stable run, compare Guard Recovery ON with the existing
Predictive Short-Cycle Control ON, keeping all other settings unchanged. Log
primary and Z2 temperatures, normal AA, guard uplift, final request/read-back,
frequency, flow and room demand. A room-demand stop is not a guard failure.
Hardware preservation of the 4 h 51 min runtime cannot be guaranteed by host tests.

## Changed source/config/test files

- `components/ecodan/ecodan.h`
- `components/ecodan/response.cpp`
- `components/optimizer/auto_adaptive.cpp`
- `components/optimizer/compressor_state.cpp`
- `components/optimizer/compressor_state.h`
- `components/optimizer/events.cpp`
- `components/optimizer/guard_recovery.cpp`
- `components/optimizer/guard_recovery.h`
- `components/optimizer/optimizer.cpp`
- `components/optimizer/optimizer.h`
- `components/optimizer/optimizer_state.h`
- `components/optimizer/prevention.cpp`
- `components/optimizer/return_guided_startup.cpp`
- `confs/auto-adaptive.yaml`
- `confs/ecodan-labels-ca.yaml`
- `confs/ecodan-labels-da.yaml`
- `confs/ecodan-labels-de.yaml`
- `confs/ecodan-labels-en.yaml`
- `confs/ecodan-labels-es.yaml`
- `confs/ecodan-labels-fi.yaml`
- `confs/ecodan-labels-fr.yaml`
- `confs/ecodan-labels-it.yaml`
- `confs/ecodan-labels-nl.yaml`
- `confs/ecodan-labels-no.yaml`
- `confs/ecodan-labels-pl.yaml`
- `confs/ecodan-labels-sk.yaml`
- `confs/ecodan-labels-sl.yaml`
- `confs/ecodan-labels-sv.yaml`
- `docs/z2-return-guided-startup.md`
- `docs/z2-startup-guard-2026-10-31.md`
- `tests/compressor_observer_test.cpp`
- `tests/guard_recovery_test.cpp`
- `tests/run_return_guided_startup.py`
- `tests/startup_aa_integration_test.cpp`
- `tests/startup_control_integration_test.cpp`
- `tests/thermostat_restart_test.cpp`
