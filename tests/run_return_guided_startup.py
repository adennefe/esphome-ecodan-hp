"""Compile and run the production startup state machine on a host compiler."""
import argparse
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
    for test in ("return_guided_startup_test", "startup_guard_integration_test", "startup_control_integration_test", "startup_aa_integration_test"):
        binary = Path(build) / (test + ".exe")
        subprocess.run(command + ["-std=c++17", "-I", str(root), "-I", build,
                       str(root / "tests" / (test + ".cpp")), "-o", str(binary)],
                       check=True, timeout=600)
        subprocess.run([str(binary)], check=True, timeout=30)
