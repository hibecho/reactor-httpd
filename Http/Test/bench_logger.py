#!/usr/bin/env python3
"""Run from repository root after make -C Http/Test bench-logger."""
import csv
import datetime
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[2]
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'tasks/logger-benchmark.csv'
    out.parent.mkdir(parents=True, exist_ok=True)
    cases = []
    for repeat, threads, payload in itertools.product(range(1, 4), (1, 4), (64, 1024)):
        for mode, queue in (('sync', 8192), ('async', 1), ('async', 8192)):
            cases.append((repeat, mode, threads, payload, queue))
    random.Random(20260927).shuffle(cases)
    binary = root / 'Http/.build/BenchLogger'
    metadata = {
        'platform': platform.platform(),
        'time_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_sha256': {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                          for name in ('Http/src/Logger.cc', 'Http/include/base/Logger.hpp',
                                       'Http/Test/BenchLogger.cc', 'Http/Test/Makefile')},
        'filesystem': subprocess.check_output(['df', '-T', '/tmp'], text=True),
        'compiler': subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
        'cpu': subprocess.check_output(['lscpu'], text=True),
        'affinity': sorted(os.sched_getaffinity(0)),
        'seed': 20260927, 'messages_per_thread': 10000,
        'note': 'Sequential processes; -O2; real temporary file, page-cache writes, no fsync; no CPU pinning.',
    }
    out.with_suffix('.environment.json').write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + '\n')
    with out.open('w', newline='') as stream:
        writer = None
        for repeat, mode, threads, payload, queue in cases:
            result = subprocess.run([str(binary), mode, str(threads), str(payload), str(queue), '10000'],
                                    check=True, text=True, capture_output=True, timeout=120)
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            if len(rows) != 1:
                raise RuntimeError(f'Unexpected benchmark output: {result.stdout}')
            row = {'repeat': repeat, **rows[0]}
            if writer is None:
                writer = csv.DictWriter(stream, fieldnames=list(row))
                writer.writeheader()
            writer.writerow(row)
            stream.flush()
            print(f'{repeat}: {mode}, threads={threads}, payload={payload}, queue={queue}', flush=True)


if __name__ == '__main__':
    main()
