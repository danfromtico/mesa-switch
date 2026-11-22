#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode a TGDB block extracted from a WDDM TDR/bugcheck driver report.

This reads the driver's 256-byte payload, not a complete Windows dump. Cached
records are independent earlier boundaries; missing/busy records prove no
GPU outcome. It does not synthesize timestamps or ETW delivery evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from decode_recovery import PAYLOAD, EXECUTION_PAYLOAD, EXECUTION_FIELDS, SOURCES, EXECUTION_SOURCES, STAGES

HEADER = struct.Struct('<8I')
RECOVERY_FIELDS = ('version', 'source', 'luid_low', 'luid_high', 'status', 'flags',
    'context', 'dma_bytes', 'private_bytes', 'umd_private_bytes', 'submission_fence',
    'last_completed_fence', 'graphics_generation', 'cached_fifo', 'cached_mmu',
    'cached_pbdma', 'cached_pbdma1')
EXECUTION_PREFIX = ('version', 'source', 'luid_low', 'luid_high', 'status',
    'request_fence', 'request_flags', 'stage', 'request_context', 'context', 'dma_address')


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated TGDB header')
    signature, version, size, written, reason, flags, fence, reserved = HEADER.unpack_from(data)
    if signature != 0x42444754 or version != 1 or size != 256 or reserved:
        raise ValueError('unsupported TGDB schema')
    if not HEADER.size <= written <= size or len(data) < written or flags & ~31:
        raise ValueError('invalid TGDB length or flags')
    if bool(flags & 4) != (written < size) or flags & 9 == 9 or flags & 18 == 18:
        raise ValueError('inconsistent TGDB validity')
    if flags & 9 and written < 104 or flags & 18 and written < 256:
        raise ValueError('TGDB claims a missing record')
    result = {'version': version, 'written_bytes': written, 'report_bytes': size,
        'reason': f'0x{reason:08x}', 'last_completed_fence': fence,
        'truncated': bool(flags & 4), 'recovery_busy': bool(flags & 8),
        'execution_busy': bool(flags & 16), 'recovery': None, 'execution': None,
        'sha256': hashlib.sha256(data[:written]).hexdigest()}
    if flags & 1:
        record = dict(zip(RECOVERY_FIELDS, PAYLOAD.unpack_from(data, HEADER.size)))
        if record['version'] != 1:
            raise ValueError('unsupported cached recovery schema')
        record['source_name'] = SOURCES[record['source']] if record['source'] < len(SOURCES) else 'unknown'
        record['status'] = f"0x{record['status']:08x}"
        result['recovery'] = record
    if flags & 2:
        record = dict(zip(EXECUTION_PREFIX + EXECUTION_FIELDS, EXECUTION_PAYLOAD.unpack_from(data, 104)))
        if record['version'] != 1:
            raise ValueError('unsupported cached execution schema')
        record['source_name'] = EXECUTION_SOURCES[record['source']] if record['source'] < len(EXECUTION_SOURCES) else 'unknown'
        record['stage_name'] = STAGES[record['stage']] if record['stage'] < len(STAGES) else 'unknown'
        record['status'] = f"0x{record['status']:08x}"
        result['execution'] = record
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.binary.read_bytes()), indent=2))
