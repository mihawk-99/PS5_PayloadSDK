#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the console capability probe's three runs (base, large, jit)."""
import re
import sys
from pathlib import Path

here = Path(__file__).resolve().parent


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def lines(name):
    return [l.split('platform-probe: ', 1)[1] for l in (here / name).read_text().splitlines()
            if 'platform-probe: ' in l]


base = lines('base.txt')
first = lambda prefix: fields(next(l for l in base if l.startswith(prefix)))
failed = [l for run in ('base.txt', 'large.txt', 'jit.txt') for l in lines(run)
          if l.startswith('check FAIL')]
assert not failed, failed
# The console and its pools.
identity = first('identity sw_version')
assert identity['sw_version_text'] == '12.090.001', identity
pool = first('pool direct_size')
assert int(pool['direct_size']) == 12 << 30
assert int(pool['flexible_configured']) == 448 << 20
largest = int(first('pool largest_allocation')['largest_allocation'])
assert largest >= 11 << 30, largest
# Execute asked for at map time is refused; RW then mprotect runs code, RX or RWX.
refusals = [fields(l) for l in base if l.startswith('exec map_time')]
assert [r['result'] for r in refusals] == ['0x80020016', '0x80020016'], refusals
assert first('exec mprotect_rx')['returned'] == '0x12345678'
rwx = first('exec mprotect_rwx')
assert rwx['mprotect_rwx_result'] == '0x00000000' and rwx['rewritten_returned'] == '0x9abcdef0'
# A mapping asked for with no address lands in the GPU window.
assert first('placement unhinted')['in_gpu_window'] == '1'
placed = [fields(l) for l in base if l.startswith('placement hint=') and 'ran=' in l]
assert any(p['ran'] == '1' and p['in_reach'] == '1' and p['in_gpu_window'] == '0' for p in placed)
# Rewriting, concurrency, faults and reuse.
assert first('rewrite one_region')['wrong'] == '0'
assert int(first('rewrite mprotect_page_ns')['mprotect_page_ns']) > 1000
context = first('fault context')
header_mcontext = int(context['mcontext_offset_words'])
header_words = {'rdi': 1, 'rsi': 2, 'rdx': 3, 'rcx': 4, 'r8': 5, 'r9': 6, 'rax': 7, 'rbp': 9,
                'r10': 10, 'r11': 11, 'r12': 12, 'r13': 13, 'r14': 14, 'r15': 15, 'rip': 20}
shifts = {r: int(context[r]) - header_mcontext - w for r, w in header_words.items()}
assert set(shifts.values()) == {6}, shifts
assert first('fault sites')['recovered'] == '200'
reuse = first('reuse cycles')
assert reuse['direct_before'] == reuse['direct_after'] and reuse['flexible_before'] == reuse['flexible_after']
# 4 GiB of guest memory beside 1 GiB of code.
large = fields(next(l for l in lines('large.txt') if l.startswith('large guest_reserve')))
assert large['ran'] == '1024' and large['guest_pages_ok'] == '4096'
assert large['flexible_before'] == large['flexible_during']
# The shared-memory JIT interface is not granted to the title.
jit = fields(next(l for l in lines('jit.txt') if l.startswith('jit ')))
assert jit['create'] == '0x80020001', jit
print('PASS: 12.09; 12 GiB pool, one allocation of', largest, 'bytes; map-time execute refused; '
      'RW then mprotect RX/RWX runs; every register six words past the header; '
      '200 faults backpatched; accounting at baseline; 4 GiB + 1 GiB; JIT interface refused')
