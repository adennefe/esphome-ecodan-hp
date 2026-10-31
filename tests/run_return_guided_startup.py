"""Compile and run the production startup state machine on a host compiler."""
import argparse
import esphome
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--cxx", help="Path to g++ or clang++")
parser.add_argument("--zig", help="Path to zig (alternative to --cxx)")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
compiler = args.cxx or shutil.which("clang++") or shutil.which("g++")
if args.zig:
    command = [args.zig, "c++", "-Wno-nullability-completeness"]
elif compiler:
    command = [compiler]
else:
    parser.error("Install a host C++ compiler, or pass --zig /path/to/zig")
with tempfile.TemporaryDirectory(prefix="return-startup-") as build:
    utility = (root / "components/optimizer/utility.cpp").read_text(encoding="utf-8")
    first = utility.index("        float Optimizer::enforce_step_limit(")
    last = utility.index("        float Optimizer::limit_external_flow(", first)
    (Path(build) / "actual_flow_limits.inc").write_text(utility[first:last], encoding="utf-8")
    adaptive = (root / "components/optimizer/auto_adaptive.cpp").read_text(encoding="utf-8")
    first = adaptive.index("        HeatingProfile Optimizer::get_heating_profile_(")
    last = adaptive.index("        //", adaptive.index("            return p;", first))
    (Path(build) / "actual_profile.inc").write_text(adaptive[first:last], encoding="utf-8")
    first = adaptive.index("        float Optimizer::calculate_heating_flow_(")
    last = adaptive.index("        float Optimizer::calculate_cooling_flow_(", first)
    (Path(build) / "actual_heating.inc").write_text(adaptive[first:last], encoding="utf-8")
    first = adaptive.index("        void Optimizer::run_auto_adaptive_loop(")
    last = adaptive.index("    } // namespace optimizer", first)
    (Path(build) / "actual_aa_loop.inc").write_text(adaptive[first:last], encoding="utf-8")
    prevention = (root / "components/optimizer/prevention.cpp").read_text(encoding="utf-8")
    first = prevention.index("        void Optimizer::predictive_short_cycle_check_for_zone_(")
    last = prevention.index("        void Optimizer::clear_predictive_boost_(", first)
    (Path(build) / "actual_predictive.inc").write_text(prevention[first:last], encoding="utf-8")
    control = (root / "components/optimizer/return_guided_startup.cpp").read_text(encoding="utf-8")
    (Path(build) / "actual_startup_control.inc").write_text(control.replace('#include "optimizer.h"', ''), encoding="utf-8")
    device_header = (root / "components/ecodan/ecodan.h").read_text(encoding="utf-8")
    first = device_header.index("        void register_binarySensor(")
    last = device_header.index("        void register_select(", first)
    (Path(build) / "actual_binary_registration.inc").write_text(
        device_header[first:last].replace("binary_sensor::BinarySensor", "Binary"), encoding="utf-8")
    response = (root / "components/ecodan/response.cpp").read_text(encoding="utf-8")
    first = response.index("        case GetType::ACTIVE_TIME:") + len("        case GetType::ACTIVE_TIME:")
    last = response.index("        case GetType::PUMP_STATUS_A:", first)
    (Path(build) / "actual_compressor_packet.inc").write_text(response[first:last], encoding="utf-8")
    events = (root / "components/optimizer/events.cpp").read_text(encoding="utf-8")
    first = events.index("        void Optimizer::on_compressor_state_change(")
    last = events.index("    } // namespace optimizer", first)
    (Path(build) / "actual_compressor_event.inc").write_text(events[first:last], encoding="utf-8")
    observer = (root / "components/optimizer/compressor_state.cpp").read_text(encoding="utf-8")
    (Path(build) / "actual_compressor_observer.inc").write_text(observer.replace('#include "optimizer.h"',''), encoding="utf-8")
    recovery = (root / "components/optimizer/guard_recovery.cpp").read_text(encoding="utf-8")
    first = recovery.index("float Optimizer::apply_z2_guard_recovery_(")
    last = recovery.index("} }", first)
    (Path(build) / "actual_guard_recovery.inc").write_text(recovery[first:last], encoding="utf-8")
    thermostat_path = Path(esphome.__file__).resolve().parent / "components/thermostat/thermostat_climate.cpp"
    thermostat = thermostat_path.read_text(encoding="utf-8")
    first = thermostat.index("bool ThermostatClimate::heating_required_()")
    last = thermostat.index("bool ThermostatClimate::supplemental_cooling_required_()", first)
    (Path(build) / "actual_thermostat_heating.inc").write_text(thermostat[first:last], encoding="utf-8")
    for test in ("return_guided_startup_test", "startup_guard_integration_test", "startup_control_integration_test", "startup_aa_integration_test", "compressor_observer_test", "guard_recovery_test", "thermostat_restart_test"):
        binary = Path(build) / (test + ".exe")
        subprocess.run(command + ["-std=c++17", "-I", str(root), "-I", build,
                       str(root / "tests" / (test + ".cpp")), "-o", str(binary)],
                       check=True, timeout=600)
        subprocess.run([str(binary)], check=True, timeout=30)
