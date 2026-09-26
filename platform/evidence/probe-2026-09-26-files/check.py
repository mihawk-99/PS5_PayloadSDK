#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the file probe's console run (files.txt, the RetroArch title writing in
its own folder): write() is fast in large chunks and slow in small ones, every
pass reads back what it wrote, and stdio is slower than all of them, the more
so the larger its buffer."""
import re
from pathlib import Path

here = Path(__file__).resolve().parent
lines = [l.split('platform-probe: files ', 1)[1]
         for l in (here / 'files.txt').read_text().splitlines()]


def rate(prefix):
    line = next(l for l in lines if l.startswith(prefix))
    assert line.endswith(' same'), line
    return float(re.search(r'write ([\d.]+) MiB/s', line).group(1))


raw = {chunk: rate(f'buffered chunk={chunk} ') for chunk in (102400, 1048576, 16777216)}
direct = rate('direct chunk=16777216 ')
stdio = {size: rate(f'stdio buffer={size} ') for size in (0, 1048576, 4194304)}
assert raw[102400] < 40 and raw[1048576] > 100 and raw[16777216] > 200, raw
assert direct > 200, direct
assert max(stdio.values()) < 20 and stdio[0] > stdio[1048576] > stdio[4194304], stdio
assert sum(1 for l in lines if l.startswith('check PASS')) == 6
assert lines[-1] == 'end failures=0', lines[-1]
print(f'PASS: write() {raw[102400]:.1f} / {raw[1048576]:.1f} / {raw[16777216]:.1f} MiB/s in '
      f'100 KiB / 1 MiB / 16 MiB chunks, O_DIRECT {direct:.1f}; fwrite() of 16 MiB '
      f'{stdio[0]:.1f} / {stdio[1048576]:.1f} / {stdio[4194304]:.1f} MiB/s with the stream\'s '
      'own buffer / 1 MiB / 4 MiB; every pass reads back what it wrote')
