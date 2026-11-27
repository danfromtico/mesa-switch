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
BREADCRUMBS_V1 = struct.Struct('<16I')
BREADCRUMB_V1_FIELDS = ('version', 'worker_stage', 'last_paging_kind', 'fault_state',
    'epoch', 'active_epoch', 'fault_epoch', 'isr_probe_enter_count',
    'isr_probe_done_count', 'isr_enter_count', 'isr_mask_count',
    'isr_unreachable_count', 'dpc_count', 'last_commit_reg',
    'commit_begin_count', 'commit_end_count')
BREADCRUMBS_V2 = struct.Struct('<HBB15I')
BREADCRUMB_V2_FIELDS = ('version', 'worker_stage', 'last_paging_kind', 'fault_state',
    'epoch', 'active_epoch', 'fault_epoch', 'isr_probe_enter_count',
    'isr_probe_done_count', 'isr_enter_count', 'isr_mask_count',
    'isr_unreachable_count', 'dpc_count', 'capture_count',
    'commit_permit_count', 'last_mmio_reg', 'mmio_enter_count', 'mmio_done_count')
COMMIT_STEP_NAMES = ('none', 'permitted_pre_mmio', 'partial_after_reg0', 'commit_done')
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
    if signature != 0x42444754 or version != 1 or size not in (256, 320) or reserved:
        raise ValueError('unsupported TGDB schema')
    if not HEADER.size <= written <= size or len(data) < written or flags & ~63:
        raise ValueError('invalid TGDB length or flags')
    has_breadcrumbs = bool(flags & 32)
    if has_breadcrumbs != (size == 320):
        raise ValueError('inconsistent TGDB breadcrumbs schema')
    if has_breadcrumbs and (written != 320 or bool(flags & 4)):
        raise ValueError('truncated TGDB breadcrumbs trailer')
    if not has_breadcrumbs and bool(flags & 4) != (written < 256):
        raise ValueError('inconsistent TGDB validity')
    if flags & 9 == 9 or flags & 18 == 18:
        raise ValueError('inconsistent TGDB validity')
    if flags & 9 and written < 104 or flags & 18 and written < 256:
        raise ValueError('TGDB claims a missing record')
    result = {'version': version, 'written_bytes': written, 'report_bytes': size,
        'reason': f'0x{reason:08x}', 'last_completed_fence': fence,
        'truncated': bool(flags & 4), 'recovery_busy': bool(flags & 8),
        'execution_busy': bool(flags & 16), 'recovery': None, 'execution': None,
        'breadcrumbs': None,
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
    if has_breadcrumbs:
        bc_ver = struct.unpack_from('<H', data, 256)[0]
        if bc_ver == 2:
            bc = dict(zip(BREADCRUMB_V2_FIELDS, BREADCRUMBS_V2.unpack_from(data, 256)))
            step = (bc['fault_state'] >> 4) & 0x3
            counts_valid = (bc['mmio_enter_count'] == bc['mmio_done_count'] or
                            bc['mmio_enter_count'] == bc['mmio_done_count'] + 1)
            uncertain = bool(bc['fault_state'] & 0x080) or not counts_valid
            in_flight = (not uncertain) and (
                bool(bc['fault_state'] & 0x040) or
                bc['mmio_enter_count'] == bc['mmio_done_count'] + 1)
            bc['worker_stage_name'] = STAGES[bc['worker_stage']] if bc['worker_stage'] < len(STAGES) else 'unknown'
            bc['last_mmio_reg'] = f"0x{bc['last_mmio_reg']:08x}"
            bc['commit_step'] = step
            bc['commit_uncertain'] = uncertain
            bc['commit_step_name'] = ('uncertain' if uncertain else
                                      'mmio_in_flight' if in_flight else
                                      COMMIT_STEP_NAMES[step])
            bc['mmio_in_flight'] = in_flight
        elif bc_ver == 1 and struct.unpack_from('<I', data, 256)[0] == 1:
            bc = dict(zip(BREADCRUMB_V1_FIELDS, BREADCRUMBS_V1.unpack_from(data, 256)))
            bc['worker_stage_name'] = STAGES[bc['worker_stage']] if bc['worker_stage'] < len(STAGES) else 'unknown'
            bc['last_commit_reg'] = f"0x{bc['last_commit_reg']:08x}"
        else:
            raise ValueError('unsupported TGDB breadcrumbs version')
        bc['armed'] = bool(bc['fault_state'] & 0x001)
        bc['faulted'] = bool(bc['fault_state'] & 0x002)
        bc['gpu_unreachable'] = bool(bc['fault_state'] & 0x004)
        bc['active_epoch_valid'] = bool(bc['fault_state'] & 0x008)
        bc['priv_sys_valid'] = bool(bc['fault_state'] & 0x100)
        bc['priv_gpc_valid'] = bool(bc['fault_state'] & 0x200)
        result['breadcrumbs'] = bc
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.binary.read_bytes()), indent=2))
