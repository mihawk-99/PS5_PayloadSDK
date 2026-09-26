#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the thread probe's console run (threads.txt, the RetroArch title's main
thread): a thread created with no attributes runs on 64 KiB, which is also what
a fresh attribute object reports, the main thread on 2 MiB, and a thread that
asks for 2 MiB gets it. The stack bases the probe prints are elided."""
import re
from pathlib import Path

here = Path(__file__).resolve().parent
lines = [l.split('platform-probe: threads ', 1)[1]
         for l in (here / 'threads.txt').read_text().splitlines()]


def stack(prefix):
    line = next(l for l in lines if l.startswith(prefix))
    return int(re.search(r'stack=(\d+) bytes', line).group(1))


default = int(re.search(r'stacksize=(\d+)', next(l for l in lines if l.startswith('default'))).group(1))
assert default == 65536, default
assert stack('no-attribute thread ') == 65536
assert stack('calling thread ') == 2 * 1024 * 1024
assert stack('2 MiB thread ') == 2 * 1024 * 1024
assert sum(1 for l in lines if l.startswith('check PASS')) == 2
assert lines[-1] == 'end failures=0', lines[-1]
print('PASS: default 64 KiB (attribute and thread), main thread 2 MiB, 2 MiB when asked')
