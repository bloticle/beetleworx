"""Summarize captured native physics cadence without claiming visual/input acceptance."""
import argparse
import json
from pathlib import Path
import re

BODY = re.compile(
    r'Native body root cadence manager ([0-9A-F]+) body ([0-9A-F]+) entity ([0-9A-F]+) '
    r'NiAV root ([0-9A-F]+): (\d+) samples / ([\d.]+)s, (\d+) comparisons, '
    r'body/root changes (\d+)/(\d+), scheduler0/1/2/3\+ step frames (\d+)/(\d+)/(\d+)/(\d+), '
    r'body changes by0/1/2/3\+ steps (\d+)/(\d+)/(\d+)/(\d+), skipped engine frames (\d+), '
    r'step ([\d.]+), residual ([\d.]+)')
INTEGRATION = re.compile(
    r'Native physics integration: (\d+) batches, (\d+) body entries / ([\d.]+)s, '
    r'dt ([\d.]+)\.\.([\d.]+), mean ([\d.]+), invalid (\d+)')


def summarize(matrix):
    source = json.loads((matrix / 'result.json').read_text(encoding='utf-8-sig'))
    result = []
    for row in source['rows']:
        if not row['observation_complete'] or not row['normal_user_files_unchanged']:
            raise ValueError('Incomplete or nonisolated timing capture')
        log = matrix / f"cap-{row['cap']}-variable-{row['variable_physics']}.log"
        text = log.read_text(encoding='utf-8-sig')
        bodies = []
        for m in BODY.finditer(text):
            if not 4.9 <= float(m[6]) <= 6:
                continue  # Exclude startup/partial windows; preserve full raw logs.
            bodies.append(dict(manager=m[1], body=m[2], entity=m[3], root=m[4],
                               samples=int(m[5]), seconds=float(m[6]), comparisons=int(m[7]),
                               body_changes=int(m[8]), root_changes=int(m[9]),
                               scheduler_frames=[int(m[i]) for i in range(10,14)],
                               changes_by_steps=[int(m[i]) for i in range(14,18)],
                               skipped_engine_frames=int(m[18]), step=float(m[19]), residual=float(m[20])))
        integration = [dict(batches=int(m[1]), body_entries=int(m[2]), seconds=float(m[3]),
                            minimum_dt=float(m[4]), maximum_dt=float(m[5]), mean_dt=float(m[6]), invalid=int(m[7]))
                       for m in INTEGRATION.finditer(text) if 4.9 <= float(m[3]) <= 6]
        if not bodies or not integration:
            raise ValueError('No complete body/integration intervals; inspect raw capture')
        frame_counts = [sum(b['scheduler_frames'][i] for b in bodies) for i in range(4)]
        result.append(dict(cap=row['cap'], variable_physics=row['variable_physics'],
                           capture_root=row['capture_root'], exe_sha256=row['exe_sha256'],
                           scheduler_body_frame_observations=frame_counts,
                           no_step_fraction=frame_counts[0]/sum(frame_counts),
                           roots_observed=any(b['root'] != '00000000' for b in bodies),
                           maximum_residual=max(b['residual'] for b in bodies),
                           integration=integration, body_intervals=bodies))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('matrices', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = [row for matrix in args.matrices for row in summarize(matrix)]
    report = dict(rows=rows, scope='Observed CPU physics/body cadence in steady diagnostic windows. '
                  'Scheduler counts repeat per observed body, not unique rendered frames. Native addresses '
                  'do not establish cross-run actor identity. Stationary bodies may repeat. No automated '
                  'movement, contact/jump acceptance, complete skeletal/root coverage or display-delivery proof.')
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps([{k: row[k] for k in ('cap','variable_physics','no_step_fraction','maximum_residual','roots_observed')}
                      for row in rows], indent=2))
