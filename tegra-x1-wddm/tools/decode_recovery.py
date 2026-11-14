#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read KMD recovery/execution events from a SWETW001 ProcessTrace export.

Input is the retained binary export, not the original ETL. Output is JSONL;
counts and input identity go to stderr. Absence of an event is not success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import uuid

PROVIDER = uuid.UUID('c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0').bytes_le
HEADER = struct.Struct('<8sQQII')
EVENT = struct.Struct('<Q16sIIHHHHBBB5x')
PAYLOAD = struct.Struct('<6IQ10I')
EXECUTION_PAYLOAD = struct.Struct('<8I3Q24I')
RESET_PACKET_HEADER = struct.Struct('<4I2Q28I')
DISPLAY_PAYLOAD = struct.Struct('<13I')
DISPLAY_OPERATIONS = ('unknown', 'commit_vidpn', 'set_timings', 'update_active_path', 'source_visibility')
RESET_HARDWARE_FIELDS = ('flags', 'mc_enable', 'pbdma_enable', 'pbdma_before', 'pbdma_after',
    'userd_get', 'userd_put', 'userd_reference', 'gp_get', 'gp_put', 'gp_fetch',
    'pb_get_lo', 'pb_get_hi', 'pb_fetch_lo', 'pb_fetch_hi', 'pb_header', 'method0',
    'data0', 'engine0', 'engine1', 'gr_interrupt', 'gr_current')
SOURCES = ('unspecified', 'submit_gate', 'submit', 'reject_drain', 'preempt',
           'worker', 'legacy_submit')
EXECUTION_SOURCES = ('unknown', 'preempt', 'worker_failure', 'submit_failure',
                     'reset_begin', 'reset_end')
STAGES = ('idle', 'software', 'paging', 'mappings', 'activate', 'prepare',
          'submit', 'gpu_fence', 'retire', 'cpu_fence', 'notify', 'preempt_prefix')
WORK_FLAGS = ('active', 'paging', 'software', 'preemption', 'rejected', 'prepared',
              'hardware_busy', 'attempted', 'submitted', 'retired', 'gpu_completed',
              'cpu_completed', 'completion_ready', 'signaling', 'notified',
              'observed_valid', 'observed_cpu', 'mc_valid', 'gr_valid')
QUEUE_FLAGS = ('engine', 'paused', 'servicing', 'preemptions_pending', 'pending',
               'recovery_pending', 'resetting', 'stopping', 'channel_fields_valid')
EXECUTION_FIELDS = ('submission_fence', 'last_completed_fence', 'expected', 'observed',
    'dma_bytes', 'work_flags', 'queue_flags', 'queue_failure', 'graphics_generation',
    'channel_stage', 'channel_register', 'channel_value', 'cached_fifo', 'cached_mmu',
    'cached_pbdma', 'cached_pbdma1', 'cached_mc', 'cached_gr', 'cached_fecs_error',
    'cached_fecs_host', 'poll_count', 'request_dma_bytes', 'private_bytes', 'umd_private_bytes')


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated export header')
    magic, expected, last, seconds, header_size = HEADER.unpack_from(data)
    if magic != b'SWETW001' or header_size != EVENT.size:
        raise ValueError('unsupported raw export format')
    offset, total, records = HEADER.size, 0, []
    while offset < len(data):
        if len(data) - offset < EVENT.size:
            raise ValueError('truncated event header')
        time, provider, pid, tid, cpu, event_id, flags, size, opcode, version, level = EVENT.unpack_from(data, offset)
        offset += EVENT.size
        if size > len(data) - offset:
            raise ValueError('truncated event payload')
        payload = data[offset:offset + size]
        offset += size
        total += 1
        if provider != PROVIDER:
            continue
        if event_id == 4:
            if version != 1 or size != DISPLAY_PAYLOAD.size:
                raise ValueError('unsupported display-path schema')
            v = DISPLAY_PAYLOAD.unpack(payload)
            if v[0] != 1 or not 1 <= v[1] <= 4 or v[5] > 1 or v[10] > 1 or v[11] > 1:
                raise ValueError('invalid display-path event')
            records.append({'event': 'DisplayPath', 'time': time, 'pid': pid, 'tid': tid, 'cpu': cpu,
                'operation': DISPLAY_OPERATIONS[v[1]], 'phase': 'return' if v[5] else 'entry',
                'adapter_luid': f'{v[3]:08x}{v[2]:08x}', 'status': f'0x{v[4]:08x}',
                'path_id': v[6], 'requested_rotation': v[7], 'before_rotation': v[8],
                'after_rotation': v[9], 'source_visible': bool(v[10]), 'target_active': bool(v[11]),
                'flags': f'0x{v[12]:08x}'})
            continue
        if event_id == 3:
            if version != 1 or size < RESET_PACKET_HEADER.size:
                raise ValueError('unsupported reset packet schema')
            v = RESET_PACKET_HEADER.unpack_from(payload)
            chunk_bytes, dma_bytes, command_bytes, chunk_offset = v[1], v[9], v[10], v[11]
            if (v[0] != 1 or chunk_bytes != size - RESET_PACKET_HEADER.size or
                    chunk_bytes > 1024 or command_bytes > 65536 or
                    (chunk_bytes | command_bytes | chunk_offset) & 3 or
                    chunk_offset + chunk_bytes > command_bytes or
                    (command_bytes and (not chunk_bytes or command_bytes != dma_bytes))):
                raise ValueError('invalid reset packet chunk')
            records.append({'event': 'ResetPacket', 'time': time, 'pid': pid, 'tid': tid, 'cpu': cpu,
                'adapter_luid': f'{v[3]:08x}{v[2]:08x}', 'context': f'0x{v[4]:016x}',
                'dma_address': f'0x{v[5]:016x}', 'submission_fence': v[6],
                'graphics_generation': v[7], 'expected': v[8], 'dma_bytes': dma_bytes,
                'command_bytes': command_bytes, 'offset': chunk_offset, 'chunk_bytes': chunk_bytes,
                'hardware': dict(zip(RESET_HARDWARE_FIELDS, v[12:])),
                'commands_hex': payload[RESET_PACKET_HEADER.size:].hex()})
            continue
        if (event_id, version, size) == (2, 1, EXECUTION_PAYLOAD.size):
            values = EXECUTION_PAYLOAD.unpack(payload)
            if values[0] != 1:
                raise ValueError('unsupported execution payload version')
            source, stage = values[1], values[7]
            record = {'event': 'ExecutionState', 'time': time, 'pid': pid, 'tid': tid, 'cpu': cpu,
                'source': source, 'source_name': EXECUTION_SOURCES[source] if source < len(EXECUTION_SOURCES) else 'unknown',
                'adapter_luid': f'{values[3]:08x}{values[2]:08x}', 'status': f'0x{values[4]:08x}',
                'request_fence': values[5], 'request_flags': f'0x{values[6]:08x}',
                'stage': stage, 'stage_name': STAGES[stage] if stage < len(STAGES) else 'unknown',
                'request_context': f'0x{values[8]:016x}', 'context': f'0x{values[9]:016x}',
                'dma_address': f'0x{values[10]:016x}'}
            record.update(zip(EXECUTION_FIELDS, values[11:]))
            record['work_state'] = [name for bit, name in enumerate(WORK_FLAGS) if record['work_flags'] & (1 << bit)]
            record['queue_state'] = [name for bit, name in enumerate(QUEUE_FLAGS) if record['queue_flags'] & (1 << bit)]
            for key in ('queue_failure', 'work_flags', 'queue_flags', 'channel_register', 'channel_value',
                        'cached_fifo', 'cached_mmu', 'cached_pbdma', 'cached_pbdma1', 'cached_mc',
                        'cached_gr', 'cached_fecs_error', 'cached_fecs_host'):
                record[key] = f'0x{record[key]:08x}'
            records.append(record)
            continue
        if (event_id, version, size) != (1, 1, PAYLOAD.size):
            raise ValueError('unsupported KMD recovery event schema')
        values = PAYLOAD.unpack(payload)
        if values[0] != 1:
            raise ValueError('unsupported recovery payload version')
        source = values[1]
        records.append({
            'event': 'FirstRecoveryRequest', 'time': time, 'pid': pid,
            'tid': tid, 'cpu': cpu, 'source': source,
            'source_name': SOURCES[source] if source < len(SOURCES) else 'unknown',
            'adapter_luid': f'{values[3]:08x}{values[2]:08x}',
            'status': f'0x{values[4]:08x}', 'flags': f'0x{values[5]:08x}',
            'context': f'0x{values[6]:016x}', 'dma_bytes': values[7],
            'private_bytes': values[8], 'umd_private_bytes': values[9],
            'submission_fence': values[10], 'last_completed_fence': values[11],
            'graphics_generation': values[12],
            'cached_fifo': f'0x{values[13]:08x}',
            'cached_mmu': f'0x{values[14]:08x}',
            'cached_pbdma': f'0x{values[15]:08x}',
            'cached_pbdma1': f'0x{values[16]:08x}',
        })
    if total != expected:
        raise ValueError(f'export event count differs: {total}/{expected}')
    return records, {'export_records': total,
                     'recovery_records': sum(r['event'] == 'FirstRecoveryRequest' for r in records),
                     'execution_records': sum(r['event'] == 'ExecutionState' for r in records),
                     'reset_packet_chunks': sum(r['event'] == 'ResetPacket' for r in records),
                     'display_records': sum(r['event'] == 'DisplayPath' for r in records),
                     'last_timestamp': last, 'requested_seconds': seconds,
                     'sha256': hashlib.sha256(data).hexdigest()}


def reconstruct_reset_packets(records):
    """Keep incomplete/ambiguous captures explicit; never fill missing words."""
    groups, current = [], {}
    identity = ('adapter_luid', 'context', 'dma_address', 'submission_fence',
                'graphics_generation', 'expected', 'dma_bytes', 'command_bytes')
    for r in records:
        if r['event'] != 'ResetPacket':
            continue
        key = tuple(r[k] for k in identity)
        if r['offset'] == 0 or key not in current:
            g = {k: r[k] for k in identity}
            g.update(time=r['time'], hardware=r['hardware'], chunks={}, consistent=True)
            groups.append(g)
            current[key] = g
        g = current[key]
        if r['offset'] in g['chunks'] or r['hardware'] != g['hardware']:
            g['consistent'] = False
        g['chunks'][r['offset']] = bytes.fromhex(r['commands_hex'])
    for g in groups:
        data, end, contiguous = [], 0, True
        for offset, chunk in sorted(g['chunks'].items()):
            contiguous &= offset == end
            data.append(chunk)
            end = offset + len(chunk)
        g['chunk_count'] = len(g.pop('chunks'))
        g['complete'] = g['consistent'] and contiguous and end == g['command_bytes'] and end > 0
        g['data'] = b''.join(data) if g['complete'] else None
        g['sha256'] = hashlib.sha256(g['data']).hexdigest() if g['complete'] else None
    return groups


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--packet-dir', type=Path, help='write only complete reset command captures')
    args = parser.parse_args()
    records, summary = decode(args.binary.read_bytes())
    if args.packet_dir:
        args.packet_dir.mkdir(parents=True, exist_ok=True)
        for i, packet in enumerate(reconstruct_reset_packets(records)):
            name = f"packet-{packet['adapter_luid']}-{packet['graphics_generation']}-{packet['submission_fence']}-{i}"
            data = packet.pop('data')
            if data is not None:
                with (args.packet_dir / (name + '.bin')).open('xb') as out:
                    out.write(data)
            with (args.packet_dir / (name + '.json')).open('x') as out:
                json.dump(packet, out, indent=2)
                out.write('\n')
    for record in records:
        print(json.dumps(record, separators=(',', ':')))
    print(json.dumps(summary), file=sys.stderr)
