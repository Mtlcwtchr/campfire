#!/usr/bin/env python3
"""Capture terrain publication morphs and distant erosion; requires diagnostics."""
import argparse
import json
import os
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=root/'build-gpu/asr_client')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/p4_runtime_continuation')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    base = [str(args.client.resolve()), '--explore', '--seed', '11', '--world', '64',
            '--at', 'mountains', '--camera', 'orbit', '--shot-time', '2']

    def ready(report):
        t = report['terrain']
        return (report['ready'] and t['pages_percent'] == t['mesh_percent'] == 100
                and not t['missing'] and t['final_stage']
                and not t['upload_failed'] and not t['capacity_limited'])

    reports = []
    for name, zoom, grid in [('mountains-far', '0.6', 'off'),
                             ('mountains-near', '6', 'off'),
                             ('mountains-mesh', '6', 'mesh')]:
        shot = out/(name+'.png')
        for frames in (900, 1800, 2700):
            command = base+['--zoom', zoom, '--grid', grid, '--shot-frame', str(frames), '--shot', str(shot)]
            print('Capture', name, frames, flush=True)
            with (out/(name+'.log')).open('w') as log:
                subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
            metadata = Path(str(shot)+'.scene.json')
            if not metadata.exists():
                raise RuntimeError('No scene metadata: build the client with ASR_ENABLE_DIAGNOSTICS=ON')
            report = json.loads(metadata.read_text())
            if ready(report):
                break
        else:
            raise RuntimeError(name+': terrain did not settle')
        reports.append(dict(name=name, command=command, screenshot=shot.name, scene=report))
        (out/'metrics.json').write_text(json.dumps(reports, indent=2)+'\n')

    command = base+['--zoom', '0.6', '--trace']
    env = dict(os.environ, ASR_VEGETATION_TRACE_LOCAL='1', ASR_VEGETATION_TRACE_SETTLE='1')
    print('Motion trace: 900 frames', flush=True)
    with (out/'motion.log').open('w') as log:
        subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    samples = [json.loads(line.split(' ', 2)[2]) for line in (out/'motion.log').read_text().splitlines()
               if line.startswith('vegetation ')]
    if not samples or not ready(samples[-1]) or any(s['terrain']['upload_failed'] for s in samples):
        raise RuntimeError('Motion trace failed readiness/upload checks')
    (out/'motion-checks.json').write_text(json.dumps(dict(command=command, frames=900,
        samples=len(samples), final=samples[-1]), indent=2)+'\n')
    print('Three captures and motion readiness passed. Not a video/art acceptance.', flush=True)


if __name__ == '__main__':
    main()
