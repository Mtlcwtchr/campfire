#!/usr/bin/env python3
"""Real-client vegetation regression captures, including an isolated grass A/B.
Uses the existing scene-model Python environment (numpy/Pillow); no desktop capture.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image


def validate(report):
    terrain = report['terrain']
    if not (report['enabled'] and report['ready'] and not report['failed']
            and terrain['pages_percent'] == terrain['mesh_percent'] == 100
            and terrain['missing'] == 0 and terrain['final_stage']
            and not terrain['capacity_limited'] and not terrain['upload_failed']):
        raise RuntimeError('Unsettled terrain or scene objects')
    grass = report['grass']
    budget = 131072 if 'far_candidates' in grass else 65536
    if not (0 <= grass['submitted_candidates'] <= grass['candidate_budget'] == budget
            and grass['submitted_triangles'] == 2 * grass['submitted_candidates']
            and report['mesh_triangles'] <= report['mesh_triangle_budget']):
        raise RuntimeError('Geometry budget/accounting failure')
    stride = grass['instance_stride_bytes']
    expected = grass['submitted_candidates'] * stride
    # Measure arena growth, not a second estimate derived from the draw count.
    # Every batch may need at most stride-1 padding bytes before its first root.
    if not (stride in (28, 32) and expected <= grass['instance_upload_bytes']
            <= expected + grass['draws'] * (stride - 1)):
        raise RuntimeError('Grass upload copied bytes as an instance count')


def image(path):
    with Image.open(path) as source:
        source.verify()
    with Image.open(path) as source:
        source.load()
        if source.size != (1280, 800):
            raise RuntimeError('Unexpected capture size')
        pixels = np.asarray(source.convert('RGB'))
        if float(pixels.std()) < 1:
            raise RuntimeError('Empty capture')
        return pixels


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=root/'build/asr_client')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/gen_rework_step_3')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.pop('ASR_GRASS_DISABLED', None)
    subprocess.run([sys.executable, str(root/'tools/capture_scene_models.py'),
                    '--client', str(args.client), '--output', str(args.output), '--frames', '720'],
                   cwd=root, env=env, check=True, timeout=1900)
    reports = json.loads((args.output/'metrics.json').read_text())
    for disabled in (False, True):
        name = 'forest-close-no-grass' if disabled else 'forest-close'
        shot = (args.output/(name+'.png')).resolve()
        command = [str(args.client.resolve()), '--explore', '--seed', '42', '--world', '64',
                   '--at', 'forest', '--camera', 'orbit', '--zoom', '12',
                   '--yaw', '0.7853981633974483', '--pitch', '0.4636476090008061',
                   '--shot-time', '2', '--shot-frame', '720', '--shot', str(shot)]
        child_env = env.copy()
        if disabled:
            child_env['ASR_GRASS_DISABLED'] = '1'
        with (args.output/(name+'.log')).open('w') as log:
            subprocess.run(command, cwd=root, env=child_env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=150)
        report = json.loads(Path(str(shot)+'.scene.json').read_text())
        validate(report)
        report.update(name=name, screenshot=shot.name, command=command, grass_disabled=disabled)
        reports.append(report)
    pictures = {}
    for report in reports:
        validate(report)
        pictures[report['name']] = image(args.output/report['screenshot'])
    a, b = reports[-2:]
    for key in ('focus_m', 'camera', 'view_projection', 'populations', 'objects', 'shader_time',
                'mesh_instances', 'impostor_instances', 'mesh_triangles'):
        if a[key] != b[key]:
            raise RuntimeError('Grass A/B changed '+key)
    if not a['grass']['enabled'] or b['grass']['enabled'] or b['grass']['submitted_candidates'] != 0:
        raise RuntimeError('Grass diagnostic toggle failed')
    changed = int(np.count_nonzero(np.any(pictures[a['name']] != pictures[b['name']], axis=2)))
    if changed == 0:
        raise RuntimeError('Grass submissions made no visible pixels')
    checks = {'grass_only_changed_pixels': changed, 'decoded_pngs': len(reports),
              'cpu_roots_are_candidates_not_visible_blade_counts': True, 'before_after': []}
    for name in ('forest-close', 'forest-near', 'forest-far'):
        before = args.output/'before'/(name+'.png')
        if not before.exists():
            continue
        old = image(before)
        old_report = json.loads(Path(str(before)+'.scene.json').read_text())
        current = next(r for r in reports if r['name'] == name)
        for key in ('focus_m', 'camera', 'view_projection', 'populations', 'objects', 'shader_time'):
            if old_report[key] != current[key]:
                raise RuntimeError('Before/after scene changed '+key)
        checks['before_after'].append({'name': name,
            'changed_pixels': int(np.count_nonzero(np.any(old != pictures[name], axis=2))),
            'before_mean_rgb': old.mean(axis=(0, 1)).tolist(),
            'after_mean_rgb': pictures[name].mean(axis=(0, 1)).tolist()})
    (args.output/'metrics.json').write_text(json.dumps(reports, indent=2)+'\n')
    (args.output/'vegetation_checks.json').write_text(json.dumps(checks, indent=2)+'\n')
    print(json.dumps(checks, indent=2), flush=True)


if __name__ == '__main__':
    main()

