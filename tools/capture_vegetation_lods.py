#!/usr/bin/env python3
"""Capture view-wide vegetation LODs, cold far views, panning and open biomes.

Also checks that the mesh level chain is the thing paying for the near frames:
a forest that spends its whole triangle budget on a few full-detail models is
the failure this was written to catch, and the totals alone do not show it.
"""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=root/'build/asr_client')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/gen_rework_step_5')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [('forest-close', 'forest', '12'), ('forest-near', 'forest', '4'),
             ('forest-far', 'forest', '0.6'), ('forest-wide', 'forest', '0.25'),
             ('forest-pan', '5642,7830', '0.6'), ('forest-return', 'forest', '0.6'),
             ('steppe', 'steppe', '4'), ('valley', 'big river', '3'), ('coast', 'coast', '4'),
             ('forest-free-low', 'forest', '12'), ('forest-free-high', 'forest', '12')]
    reports = []
    for name, place, zoom in cases:
        shot = (args.output/(name+'.png')).resolve()
        command = [str(args.client.resolve()), '--explore', '--seed', '42', '--world', '64',
                   '--at', place, '--camera', 'orbit', '--zoom', zoom, '--shot-time', '2',
                   '--shot-frame', '720', '--shot', str(shot)]
        if name.startswith('forest-free-'):
            command[command.index('--camera')+1] = 'free'
            command += ['--height-offset', '80' if name.endswith('low') else '2000']
        print('Capturing', name, flush=True)
        for attempt in range(3):
            command[command.index('--shot-frame')+1] = str(720*(attempt+1))
            with (args.output/(name+'.log')).open('w') as log:
                subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=180)
            r = json.loads(Path(str(shot)+'.scene.json').read_text())
            t = r['terrain']
            if (r['ready'] and t['pages_percent'] == t['mesh_percent'] == 100 and not t['missing']
                    and t['final_stage'] and not t['upload_failed'] and not t['capacity_limited']):
                break
        else:
            raise RuntimeError(name+': did not settle')
        g = r['grass']
        if not (r['view_wide_vegetation'] and r['grove_impostors'] <= r['grove_budget']
                and r['mesh_triangles'] <= r['mesh_triangle_budget']
                and g['submitted_candidates'] <= g['candidate_budget']
                and g['submitted_candidates'] == g['near_candidates']+g['far_candidates']):
            raise RuntimeError(name+': invalid LOD/budget counters')
        expected = g['submitted_candidates']*g['instance_stride_bytes']
        if not expected <= g['instance_upload_bytes'] <= expected+g['draws']*(g['instance_stride_bytes']-1):
            raise RuntimeError(name+': arena count/bytes regression')
        if g.get('gpu_frustum_culling'):
            if not (0 <= g['drawn_candidates'] <= g['submitted_candidates']
                    and g['drawn_triangles'] == 2*g['drawn_candidates']
                    and g['draws'] == int(g['submitted_candidates'] > 0)
                    and g['gpu_indirect_draw'] == bool(g['draws'])):
                raise RuntimeError(name+': invalid GPU grass compaction/indirect draw')
        if name in ('forest-far', 'forest-wide', 'forest-pan', 'forest-return', 'forest-free-high'):
            # The elevated free camera faces a different part of the woodland.
            minimum_groves, minimum_blocks = (1, 1) if name == 'forest-free-high' else (100, 10)
            if (r['detail_requested'] or r['sampled_sites'] or g['near_candidates']
                    or r['grove_impostors'] < minimum_groves or r['grove_blocks'] < minimum_blocks
                    or r['grove_max_focus_distance_m'] <= 768 or g['far_blocks'] < 10):
                raise RuntimeError(name+': vegetation still restricted to the local window')
        if name in ('forest-close', 'forest-free-low') and (not r['detail_requested'] or not r['sampled_sites'] or not r['mesh_instances']):
            raise RuntimeError('Near detail did not replace the proxies')
        chains = r['mesh_level_triangle_counts']
        if (sum(r['mesh_level_instances']) != r['mesh_instances']
                or sum(r['mesh_level_triangles']) != r['mesh_triangles']
                or any(list(c) != sorted(c, reverse=True) or len(set(c)) != len(c) for c in chains)):
            raise RuntimeError(name+': mesh level accounting does not add up')
        if r['mesh_instances']:
            # The budget must no longer be what decides which objects get
            # geometry, and the average object must cost far less than the
            # imported mesh - otherwise the levels exist and are not used.
            average = r['mesh_triangles']/r['mesh_instances']
            if (r['lod_mesh_pixels'][0] != 22 or average > max(c[0] for c in chains)/4
                    or r['mesh_level_instances'][0] > r['mesh_instances']//10):
                raise RuntimeError(name+': mesh levels are not carrying the frame')
        r.update(name=name, command=command, screenshot=shot.name)
        reports.append(r)
        (args.output/'metrics.json').write_text(json.dumps(reports, indent=2)+'\n')
        print(name, 'groves', r['grove_impostors'], 'blocks', r['grove_blocks'],
              'reach', round(r['grove_max_focus_distance_m']), 'fine sites', r['sampled_sites'],
              'meshes', r['mesh_instances'], 'by level', r['mesh_level_instances'],
              'mesh triangles', r['mesh_triangles'], flush=True)
    print('All view-wide vegetation LOD checks passed.', flush=True)


if __name__ == '__main__':
    main()

