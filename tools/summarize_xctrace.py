#!/usr/bin/env python3
"""Summarize exported xctrace time-profile XML, including interned element refs.
Weights are sampled CPU time, not exact wall-time or hardware cycle counts.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def summarize(path):
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
    total = 0
    samples = 0
    for row in root.iter('row'):
        weight = resolve(row.find('weight'))
        stack = resolve(row.find('tagged-backtrace'))
        if weight is None or stack is None:
            continue
        ns = int(weight.text or 0)
        if ns <= 0: continue
        trace = resolve(stack.find('backtrace'))
        if trace is None: continue
        names = [resolve(f).attrib.get('name', '<unknown>') for f in trace
                 if f.tag == 'frame' and resolve(f) is not None]
        if not names: continue
        samples += 1; total += ns
        self_time[names[0]] += ns
        for name in set(names): inclusive[name] += ns
        thread = resolve(row.find('thread'))
        threads[thread.attrib.get('fmt', 'unknown') if thread is not None else 'unknown'] += ns
    def table(values, count=20):
        return [{'name': name, 'sampled_ms': round(ns / 1e6, 3),
                 'percent_of_sampled_cpu': round(100 * ns / max(total, 1), 2)}
                for name, ns in values.most_common(count)]
    return {'samples': samples, 'sampled_cpu_ms': round(total / 1e6, 3),
            'self': table(self_time), 'inclusive': table(inclusive), 'threads': table(threads)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = summarize(args.input)
    text = json.dumps(result, indent=2)
    if args.out: args.out.write_text(text + '\n')
    else: print(text)
