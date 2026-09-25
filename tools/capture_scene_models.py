#!/usr/bin/env python3
"""Capture bounded real-client runs for terrain-decoration regression checks.
No desktop capture and no interaction with other running client processes.
"""
import argparse
import json
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument('--client', type=Path, default=root/'build/asr_client')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/gen_rework_step_1')
    parser.add_argument('--frames', type=int, default=360)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [('forest-near', 'forest', '4', '2'), ('forest-far', 'forest', '0.6', '2'),
             ('mountains', 'mountains', '3', '2'), ('coast', 'coast', '4', '2'),
             ('forest-wind', 'forest', '4', '6')]
    results = []
    for name, landmark, zoom, seconds in cases:
        shot = args.output/(name+'.png')
        command = [str(args.client.resolve()), '--explore', '--seed', '42', '--world', '64',
                   '--at', landmark, '--camera', 'orbit', '--zoom', zoom,
                   '--yaw', '0.7853981633974483', '--pitch', '0.4636476090008061',
                   '--shot-time', seconds, '--shot-frame', str(args.frames), '--shot', str(shot.resolve())]
        print('Capturing', name, flush=True)
        start = time.monotonic()
        for attempt in range(3):
            command[command.index('--shot-frame')+1] = str(args.frames*(attempt+1))
            with (args.output/(name+'.log')).open('w') as log:
                subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=args.timeout)
            report = json.loads(Path(str(shot)+'.scene.json').read_text())
            terrain = report['terrain']
            settled = (terrain['pages_percent'] == 100 and terrain['mesh_percent'] == 100
                       and terrain['missing'] == 0 and not terrain['capacity_limited']
                       and not terrain['upload_failed'] and terrain['final_stage'])
            if report['ready'] and settled:
                break
            print('Not settled:', name, 'attempt', attempt+1, flush=True)
        else:
            raise RuntimeError(name+': terrain/objects still not ready after three bounded runs')
        if not report['enabled'] or not report['ready'] or report['failed']:
            raise RuntimeError(name+': scene objects not ready')
        if report['draws'] > 6 or report['terrain']['upload_failed']:
            raise RuntimeError(name+': draw budget or upload failure')
        if report['mesh_triangles'] > report['mesh_triangle_budget']:
            raise RuntimeError(name+': mesh triangle budget exceeded')
        if sum(report['populations']) != report['objects']:
            raise RuntimeError(name+': inconsistent scatter populations')
        if not shot.is_file() or shot.stat().st_size < 1024:
            raise RuntimeError(name+': invalid screenshot')
        report.update(name=name, screenshot=shot.name, command=command,
                      run_seconds=time.monotonic()-start, screenshot_bytes=shot.stat().st_size)
        results.append(report)
        # Preserve intermediate results even if a later graphical run times out.
        (args.output/'metrics.json').write_text(json.dumps(results, indent=2)+'\n')
        print(name, 'mesh:', report['mesh_instances'], 'impostors:', report['impostor_instances'],
              'draws:', report['draws'], 'terrain mesh %:', report['terrain']['mesh_percent'], flush=True)
    near, far = results[:2]
    if near['mesh_instances'] <= 0 or far['impostor_instances'] <= 0:
        raise RuntimeError('both representations must actually be drawn')
    if far['triangles'] >= near['triangles']:
        raise RuntimeError('distant LOD did not reduce submitted triangles')
    if near['focus_m'] != results[-1]['focus_m'] or near['objects'] != results[-1]['objects']:
        raise RuntimeError('wind comparison changed the scene')
    print('All client runs finished; see metrics.json for readiness and limits.', flush=True)


if __name__ == '__main__':
    main()

