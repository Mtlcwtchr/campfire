#!/usr/bin/env python3
"""Summarize exported xctrace time-profile XML, including interned element refs.
Weights are sampled CPU time, not exact wall-time or hardware cycle counts.

Besides the flat tables it reads the timeline:
- spikes: where the main thread stayed inside one piece of work without a
  break for longer than a frame can afford - the deepest function covering the
  stretch, how long, when, and the stack above it;
- callers: for the hottest functions by self time, the chains that call them,
  so an unnamed or generic frame (memmove, a lambda) says whose it is.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import xml.etree.ElementTree as ET

# Frames every main-thread sample carries are the loop, not the work.
UBIQUITY = 0.6
# Samples further apart than this are separate stretches of work.
GAP_NS = 3_000_000


def summarize(path, spike_ms=25.0, spikes=25, callers=12):
    root = ET.parse(path).getroot()
    ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}

    def resolve(e):
        seen = set()
        while e is not None and 'ref' in e.attrib:
            key = e.attrib['ref']
            if key in seen:
                raise ValueError('Cyclic xctrace reference')
            seen.add(key)
            e = ids.get(key)
        return e

    inclusive, self_time, threads = Counter(), Counter(), Counter()
    caller_chains = defaultdict(Counter)
    main = []   # (time ns, weight ns, names leaf-first)
    total = 0
    samples = 0
    for row in root.iter('row'):
        weight = resolve(row.find('weight'))
        stack = resolve(row.find('tagged-backtrace'))
        if weight is None or stack is None:
            continue
        ns = int(weight.text or 0)
        if ns <= 0:
            continue
        trace = resolve(stack.find('backtrace'))
        if trace is None:
            continue
        names = [resolve(f).attrib.get('name', '<unknown>') for f in trace
                 if f.tag == 'frame' and resolve(f) is not None]
        if not names:
            continue
        samples += 1
        total += ns
        self_time[names[0]] += ns
        caller_chains[names[0]][tuple(names[1:4])] += ns
        for name in set(names):
            inclusive[name] += ns
        thread = resolve(row.find('thread'))
        label = thread.attrib.get('fmt', 'unknown') if thread is not None else 'unknown'
        threads[label] += ns
        when = resolve(row.find('sample-time'))
        if label.startswith('Main Thread') and when is not None:
            main.append((int(when.text or 0), ns, names))

    def table(values, count=20):
        return [{'name': name, 'sampled_ms': round(ns / 1e6, 3),
                 'percent_of_sampled_cpu': round(100 * ns / max(total, 1), 2)}
                for name, ns in values.most_common(count)]

    return {'samples': samples, 'sampled_cpu_ms': round(total / 1e6, 3),
            'self': table(self_time), 'inclusive': table(inclusive), 'threads': table(threads),
            'spikes': find_spikes(main, spike_ms, spikes),
            'callers': [{'name': name, 'sampled_ms': round(ns / 1e6, 3),
                         'chains': [{'via': list(chain), 'sampled_ms': round(w / 1e6, 3)}
                                    for chain, w in caller_chains[name].most_common(3)]}
                        for name, ns in self_time.most_common(callers)]}


def find_spikes(main, spike_ms, count):
    """The longest unbroken stretches of the main thread inside one function."""
    main.sort(key=lambda s: s[0])
    if not main:
        return []
    presence = Counter()
    for _, _, names in main:
        presence.update(set(names))
    loop = {name for name, n in presence.items() if n > UBIQUITY * len(main)}
    runs = []          # (start, end, depth-from-root, name, stack)
    open_runs = {}     # name -> [start, last, depth, stack]
    for when, weight, names in main:
        present = {}
        for depth, name in enumerate(reversed(names)):   # root first: depth grows inwards
            if name not in loop and name not in present:
                present[name] = depth
        for name in list(open_runs):
            run = open_runs[name]
            if name not in present or when - run[1] > GAP_NS:
                runs.append((run[0], run[1], run[2], name, run[3]))
                del open_runs[name]
        for name, depth in present.items():
            if name in open_runs:
                open_runs[name][1] = when + weight
            else:
                open_runs[name] = [when, when + weight, depth,
                                   [n for n in reversed(names) if n not in loop][:depth + 1][-8:]]
    for name, run in open_runs.items():
        runs.append((run[0], run[1], run[2], name, run[3]))
    threshold = spike_ms * 1e6
    long = [r for r in runs if r[1] - r[0] >= threshold]
    # Overlapping stretches are one spike seen through its ancestors: keep the
    # deepest function that covers most of it.
    long.sort(key=lambda r: (r[0], -(r[1] - r[0])))
    groups = []
    for r in long:
        if groups and r[0] < groups[-1]['end']:
            g = groups[-1]
            g['end'] = max(g['end'], r[1])
            g['runs'].append(r)
        else:
            groups.append({'start': r[0], 'end': r[1], 'runs': [r]})
    result = []
    for g in groups:
        longest = max(r[1] - r[0] for r in g['runs'])
        best = max((r for r in g['runs'] if r[1] - r[0] >= 0.8 * longest), key=lambda r: r[2])
        result.append({'at_seconds': round(best[0] / 1e9, 3), 'ms': round((best[1] - best[0]) / 1e6, 1),
                       'function': best[3], 'stack': best[4]})
    result.sort(key=lambda s: -s['ms'])
    return result[:count]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--spike-ms', type=float, default=25.0,
                        help='shortest unbroken main-thread stretch reported as a spike')
    args = parser.parse_args()
    result = summarize(args.input, args.spike_ms)
    text = json.dumps(result, indent=2)
    if args.out:
        args.out.write_text(text + '\n')
    else:
        print(text)
