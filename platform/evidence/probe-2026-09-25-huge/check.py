#!/usr/bin/env python3
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the runs built with this fork's machine-context header.

reserve-refused.txt: the first 10 GiB test, whose reservation was refused, kept
because it established that behaviour. survey.txt: the survey of virtual space
and the 10 GiB mapped and written at once. full.txt: the largest allocation
mapped and written, and what is left while it is held.
"""
import re
from pathlib import Path

here = Path(__file__).resolve().parent
GIB = 1 << 30


def fields(line):
    return dict(re.findall(r'(\w+)=(\S+)', line))


def lines(name):
    return [l.split('platform-probe: ', 1)[1] for l in (here / name).read_text().splitlines()
            if 'platform-probe: ' in l]


def first(run, prefix):
    return fields(next(l for l in run if l.startswith(prefix)))


refused, survey, full = lines('reserve-refused.txt'), lines('survey.txt'), lines('full.txt')
for run in (refused, survey, full):
    assert first(run, 'identity sw_version')['sw_version_text'] == '12.090.001'
    # The header matches the console: uc_mcontext at word 8, every marker at
    # its FreeBSD mcontext field.
    context = first(run, 'fault context')
    assert context['mcontext_offset_words'] == '8', context
    words = {'rdi': 1, 'rsi': 2, 'rdx': 3, 'rcx': 4, 'r8': 5, 'r9': 6, 'rax': 7, 'rbp': 9,
             'r10': 10, 'r11': 11, 'r12': 12, 'r13': 13, 'r14': 14, 'r15': 15, 'rip': 20}
    assert all(int(context[r]) == 8 + w for r, w in words.items()), context
    end = first(run, 'end failures')
    assert end['flexible_start'] == end['flexible_end'] and end['direct_start'] == end['direct_end']

# Every check of the base tests passed in both; the first run's only failures
# are the four 10 GiB checks its refused reservation left without memory.
failed = [l for l in refused if l.startswith('check FAIL')]
assert len(failed) == 4 and all('10 GiB' in l or 'ten' in l for l in failed), failed
single = first(refused, 'huge single bytes')
assert single['reserve'] == '0x8002000c' and single['view'] == '600000000', single
assert not [l for l in survey if l.startswith('check FAIL')]
assert first(survey, 'end failures')['failures'] == '0'

# Virtual space.
surveyed = {int(f['hint'], 16): f for f in (fields(l) for l in survey if l.startswith('huge survey hint='))}
assert int(surveyed[0x400000000]['largest']) == 15 * GIB
assert int(surveyed[0x600000000]['largest']) == 7 * GIB
moved = surveyed[0x800000000]
assert int(moved['largest']) == 16 * GIB and moved['at'] == '0xfe0480000', moved
for hint in (0x1000000000, 0x2000000000, 0x4000000000, 0x8000000000):
    assert int(surveyed[hint]['largest']) == 16 * GIB and int(surveyed[hint]['at'], 16) == hint
for hint in (0x10000000000, 0x40000000000, 0x100000000000, 0x400000000000):
    assert surveyed[hint]['largest'] == '0' and surveyed[hint]['refusal'] == '0x8002000c'

# 10 GiB at once, as one allocation and as ten.
single = first(survey, 'huge single bytes')
assert int(single['bytes']) == 10 * GIB and single['map'] == '0x00000000'
assert single['wrong_words'] == '0' and single['in_gpu_window'] == '0'
assert single['flexible_before'] == single['flexible_during']
assert int(single['direct_before']) - int(single['direct_during']) >= 10 * GIB
assert first(survey, 'huge single direct_after')['direct_after'] == single['direct_before']
pieces = first(survey, 'huge pieces pieces')
assert pieces['mapped'] == '10' and pieces['wrong_words'] == '0' and pieces['in_gpu_window'] == '0'
assert pieces['flexible_before'] == pieces['flexible_during']

# The whole pool: all of the free direct memory is one allocatable block, the
# largest allocation found (to 64 MiB) is mapped and every word checked, and
# what is left while it is held is the part below the search step.
baseline = first(full, 'full baseline')
assert baseline['allocatable'] == baseline['largest_block'] == first(full, 'pool direct_size')['direct_available']
held = first(full, 'full bytes')
assert held['bytes'] == first(full, 'pool largest_allocation')['largest_allocation']
assert int(held['bytes']) >= 11 * GIB + 3 * GIB // 4
assert held['map'] == '0x00000000' and held['wrong_words'] == '0' and held['in_gpu_window'] == '0'
assert held['flexible_before'] == held['flexible_during']
left = first(full, 'full left')
assert int(held['bytes']) + int(left['allocatable']) == int(baseline['allocatable'])
assert int(left['allocatable']) < 64 << 20 and left['flexible'] == held['flexible_before']
assert first(full, 'full direct_after')['direct_after'] == baseline['allocatable']
assert not [l for l in full if l.startswith('check FAIL')]
print('PASS: header matches the console (uc_mcontext at word 8, rip at 28); a 10 GiB '
      'reservation refused at 0x6_0000_0000; 16 GiB reserved at hints 0x10_0000_0000-'
      '0x80_0000_0000, nothing from 1 TiB; 10 GiB mapped, written and read back as one '
      'allocation (fill', int(single['fill_ns']) // 1000000, 'ms, verify',
      int(single['verify_ns']) // 1000000, 'ms) and as ten, flexible memory unchanged; '
      'the largest allocation,', held['bytes'], 'bytes, mapped and written with',
      left['allocatable'], 'bytes of direct memory left')
