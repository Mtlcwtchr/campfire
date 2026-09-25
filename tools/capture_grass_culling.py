#!/usr/bin/env python3
"""Same-process GPU/direct/GPU grass A/B/A; no FPS claim or desktop capture.

Uses the existing numpy/Pillow environment documented in doc/scene_models.md.
The baseline switches off only GPU grass culling, never grass or terrain cover.
The repeated GPU frame must be bit-identical, proving that the scene did not drift.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image


def pixels(path):
    with Image.open(path) as image:
        image.verify()
    with Image.open(path) as image:
        data = np.asarray(image.convert('RGB'))
    if data.shape != (800, 1280, 3) or float(data.std()) < 1:
        raise RuntimeError('Invalid/empty screenshot: ' + str(path))
    return data


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=root/'build/asr_client')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/vg_step_2')
    parser.add_argument('--check-only', action='store_true', help='Validate already captured A/B files')
    args = parser.parse_args()
    output = args.output.resolve()
    if not args.check_only:
        env = os.environ.copy()
        env.pop('ASR_GRASS_DISABLED', None)
        env.pop('ASR_GRASS_CULL_DISABLED', None)
        env['ASR_GRASS_COMPARE_CULL'] = '1'
        subprocess.run([sys.executable, str(root/'tools/capture_vegetation_lods.py'),
                        '--client', str(args.client.resolve()), '--output', str(output)],
                       cwd=root, env=env, check=True, timeout=6500)
    after = json.loads((output/'metrics.json').read_text())
    if len(after) != 11 or len({r['name'] for r in after}) != 11:
        raise RuntimeError('Incomplete 11-scenario A/B/A')
    checks = []
    for report in after:
        direct_path = output/(report['screenshot']+'.direct.png')
        repeat_path = output/(report['screenshot']+'.repeat.png')
        old = json.loads(Path(str(direct_path)+'.scene.json').read_text())
        repeat = json.loads(Path(str(repeat_path)+'.scene.json').read_text())
        for key in ('seed', 'world_cells', 'world_metres', 'focus_m', 'camera', 'view_projection',
                    'viewport', 'shader_time', 'wind', 'populations', 'objects', 'mesh_instances',
                    'mesh_triangles', 'mesh_level_instances', 'grove_impostors'):
            if old[key] != report[key] or repeat[key] != report[key]:
                raise RuntimeError(report['name'] + ': A/B changed ' + key)
        a, b = old['grass'], report['grass']
        for key in ('submitted_candidates', 'near_candidates', 'far_candidates', 'instance_upload_bytes',
                    'instance_stride_bytes', 'draws', 'far_blocks'):
            if a[key] != b[key]:
                raise RuntimeError(report['name'] + ': A/B changed grass ' + key)
        if (a['gpu_frustum_culling'] or not b['gpu_frustum_culling']
                or a['drawn_candidates'] != a['submitted_candidates']
                or not 0 < b['drawn_candidates'] <= b['submitted_candidates']
                or not b['gpu_indirect_draw'] or b['drawn_triangles'] != b['drawn_candidates']*2):
            raise RuntimeError(report['name'] + ': culling mode/count mismatch')
        x, y, z = pixels(direct_path), pixels(output/report['screenshot']), pixels(repeat_path)
        changed = int(np.count_nonzero(np.any(x != y, axis=2)))
        repeated = int(np.count_nonzero(np.any(z != y, axis=2)))
        checks.append({'name': report['name'], 'source_candidates': b['submitted_candidates'],
                       'drawn_candidates': b['drawn_candidates'],
                       'culled_candidates': b['submitted_candidates']-b['drawn_candidates'],
                       'changed_pixels': changed, 'repeat_changed_pixels': repeated,
                       'total_pixels': 1280*800,
                       'max_channel_difference': int(np.abs(x.astype(np.int16)-y).max()),
                       'gpu_cull_working_bytes': b['gpu_cull_working_bytes']})
    result = {'scenarios': len(checks), 'decoded_pngs': 3*len(checks), 'same_process_aba': True,
              'source_candidates': sum(c['source_candidates'] for c in checks),
              'drawn_candidates': sum(c['drawn_candidates'] for c in checks),
              'checks': checks}
    (output/'culling-checks.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)
    if result['drawn_candidates'] >= result['source_candidates']:
        raise RuntimeError('GPU culling removed no work')
    if any(c['repeat_changed_pixels'] for c in checks):
        raise RuntimeError('Repeated GPU frame differs: A/B/A scene is not stable')
    # Only sub-pixel tie differences are allowed, not a vanished grass layer.
    if any(c['changed_pixels'] > c['total_pixels']*0.0001 for c in checks):
        raise RuntimeError('Grass A/B changed more than 0.01% of a frame; inspect culling-checks.json')


if __name__ == '__main__':
    main()

