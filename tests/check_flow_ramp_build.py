"""Audit generated ESPHome registrations and byte-identical local optimizer sources."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('build_path', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
main = (args.build_path/'src/main.cpp').read_text(encoding='utf-8')
checks = 0
def check(ok, message):
    global checks
    checks += 1
    if not ok:
        raise AssertionError(message)

check('z2_flow_anchored_startup->set_restore_mode(switch_::SWITCH_ALWAYS_OFF)' in main,
      'New switch must not restore ON across an ESP restart')
check('App.register_switch(z2_flow_anchored_startup,' in main, 'Switch registered')
for entity, initial in (('z2_flow_ramp_initial_delta', '2.0f'), ('z2_flow_ramp_step', '0.5f'),
                        ('z2_flow_ramp_interval', '5.0f'), ('z2_startup_duration', '45.0f')):
    check(f'App.register_number({entity},' in main, f'{entity}: number registration')
    check(f'{entity}->set_initial_value({initial})' in main, f'{entity}: initial value')
    check(f'{entity}->set_restore_value(true)' in main, f'{entity}: retain user configuration')
for entity in ('z2_flow_anchored_startup', 'z2_flow_ramp_initial_delta', 'z2_flow_ramp_step', 'z2_flow_ramp_interval'):
    check(f'state.{entity} = {entity};' in main, f'{entity}: optimizer wiring')
check('minimum_heating_flow_temp_z2->set_initial_value(24.0f)' in main, 'Ordinary Z2 minimum')
check('asgard-z2-en-2026-11-07.01-flow-anchored-ramp-1' in main, 'Firmware identity')
check('gekkekoe/esphome-ecodan-hp@main' not in main, 'No upstream external component source')
for component in ('optimizer', 'ecodan'):
    for source in (root/'components'/component).iterdir():
        if source.suffix not in ('.cpp', '.h'):
            continue
        generated = args.build_path/'src/esphome/components'/component/source.name
        check(generated.read_bytes() == source.read_bytes(), f'Local source identity: {source.name}')
print(f'PASS: {checks} generated entity/configuration/local-source checks.')
