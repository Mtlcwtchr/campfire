#!/usr/bin/env python3
"""Validate the step-4 forest --trace route: continuous pan, zoom and LOD fallback."""
import argparse
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, default=root/'build/gen-rework-step-5-motion.log')
    parser.add_argument('--output', type=Path, default=root/'doc/reports/gen_rework_step_5/motion-checks.json')
    parser.add_argument('--frames', type=int, default=900)
    args = parser.parse_args()
    rows = []
    for line in args.log.read_text().splitlines():
        if not line.startswith('vegetation '):
            continue
        _, frame, payload = line.split(' ', 2)
        rows.append((int(frame), json.loads(payload)))
    if len(rows) < 50 or rows[-1][0] != args.frames:
        raise RuntimeError('Incomplete vegetation trace')
    fallback = []
    empty = []
    for frame, report in rows:
        grass = report['grass']
        if not report['view_wide_vegetation'] or report['failed']:
            raise RuntimeError('Vegetation unavailable at frame '+str(frame))
        if (report['mesh_triangles'] > report['mesh_triangle_budget']
                or report['grove_impostors'] > report['grove_budget']
                or grass['submitted_candidates'] > grass['candidate_budget']
                or grass['near_candidates']+grass['far_candidates'] != grass['submitted_candidates']):
            raise RuntimeError('Geometry budget failure at frame '+str(frame))
        stride = grass['instance_stride_bytes']
        expected = grass['submitted_candidates']*stride
        # Historical roots: 28 bytes; VG1 adds the four-byte packed run word.
        if stride not in (28, 32) or not expected <= grass['instance_upload_bytes'] <= expected+grass['draws']*(stride-1):
            raise RuntimeError('Instance upload regression at frame '+str(frame))
        if report['detail_requested'] and not report['ready'] and report['grove_impostors'] > 0:
            fallback.append(frame)
        # The level chain has to hold during movement too, not only in the
        # settled frames the screenshots are taken from.
        if (sum(report['mesh_level_instances']) != report['mesh_instances']
                or sum(report['mesh_level_triangles']) != report['mesh_triangles']):
            raise RuntimeError('Mesh level accounting broke at frame '+str(frame))
        if frame >= 90 and report['grove_impostors']+report['impostor_instances']+report['mesh_instances'] == 0:
            empty.append(frame)
    requested = [f for f, r in rows if r['detail_requested']]
    exact_ready = [f for f, r in rows if r['detail_requested'] and r['ready']]
    distance = max(r['focus_m'][0] for _, r in rows)-min(r['focus_m'][0] for _, r in rows)
    if not requested or not any(not r['detail_requested'] for _, r in rows) or distance < 256:
        raise RuntimeError('Route did not exercise pan and exact-detail activation')
    if empty or not fallback:
        raise RuntimeError('Missing forest fallback during movement: '+str(empty))
    if args.frames > 300 and not exact_ready:
        raise RuntimeError('Exact-detail replacement did not finish during the held-camera tail')
    result = {'frames': args.frames, 'sampled_frames': len(rows), 'pan_metres_x': distance,
              'first_exact_request_frame': requested[0],
              'first_ready_exact_frame': exact_ready[0] if exact_ready else None,
              'pending_exact_with_grove_fallback_frames': fallback,
              'empty_vegetation_frames_after_settle': empty,
              'all_sampled_budgets_valid': True,
              'grove_count_min_after_settle': min(r['grove_impostors'] for f, r in rows if f >= 90),
              'grove_count_max': max(r['grove_impostors'] for _, r in rows),
              'maximum_grass_upload_bytes': max(r['grass']['instance_upload_bytes'] for _, r in rows),
              'maximum_mesh_instances': max(r['mesh_instances'] for _, r in rows),
              'maximum_mesh_triangles': max(r['mesh_triangles'] for _, r in rows),
              'maximum_full_detail_instances': max(r['mesh_level_instances'][0] for _, r in rows),
              'mesh_level_pixel_error': rows[0][1]['mesh_level_pixel_error']}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

