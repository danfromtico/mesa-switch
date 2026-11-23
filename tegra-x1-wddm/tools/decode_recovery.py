#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read KMD recovery/execution/display events from retained trace exports.

Default input is SWETW001, not the original ETL. --live-jsonl reads the
--follow-all export without converting QPC ticks into wall-clock time.
Output is JSONL; counts and input identity go to stderr. Missing records,
including a missing return, do not establish success or a hung DDI.
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
TDR_PAYLOAD = struct.Struct('<4IQ2Ii6Ii')
CONTEXT_PAYLOAD = struct.Struct('<3Ii4Q8IiIQ6I')
CONTEXT_OPERATIONS = ('unknown', 'create', 'destroy', 'set_root', 'prepare',
                      'paging_dma_complete', 'paging_capture_failure')
CONTEXT_DECISIONS = ('none', 'engine_unavailable', 'invalid_context', 'root_unavailable',
                     'root_status_failed', 'graphics_unavailable', 'stale_generation')
CONTEXT_FLAGS = ('found', 'process_present', 'adapter_identity_valid', 'root_valid', 'rebuilding',
                 'gpu_completed', 'cpu_completed', 'retired', 'observed_valid', 'observed_cpu',
                 'system_context', 'virtual_addressing', 'gdi_context', 'system_process',
                 'recovery_pending', 'resetting', 'stopping', 'graphics_ready', 'closing',
                 'work_root_valid', 'work_root_matches_canonical', 'work_snapshot',
                 'submitted', 'hardware_busy', 'attempted', 'paging')
TDR_OPERATIONS = ('unknown', 'reset_adapter', 'restart_adapter', 'query_engine', 'reset_engine', 'query_current_fence')
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


def _decode_event(event_id, version, payload, metadata):
    """Decode the same fixed KMD payload for binary and live exports."""
    size = len(payload)
    if event_id == 9:
        if version != 1 or size != CONTEXT_PAYLOAD.size:
            raise ValueError('unsupported context evidence schema')
        v = CONTEXT_PAYLOAD.unpack(payload)
        operation, flags, decision = v[1], v[2], v[23]
        if (v[0] != 1 or not 1 <= operation <= 6 or flags & ~0x3ffffff or
                v[24] or not 0 <= decision < len(CONTEXT_DECISIONS) or
                ((operation == 6) != (decision != 0)) or
                (operation == 6 and v[3] >= 0) or
                (operation != 6 and not flags & 1) or
                (flags & 8192 and not flags & 2) or
                (flags & (2 | 8 | 1024 | 2048 | 4096 | 262144 | 2097152) and not flags & 1) or
                (flags & (32 | 64 | 128 | 256 | 512 | 524288 | 1048576 | 4194304 | 8388608 | 16777216) and not flags & 2097152) or
                (operation == 4 and not flags & 2097152) or
                (operation in (1, 2, 3, 6) and flags & 2097152) or
                (flags & 16 and operation != 4) or
                (flags & 1048576 and flags & (8 | 524288 | 2097152) != (8 | 524288 | 2097152))):
            raise ValueError('invalid context evidence validity flags')
        if operation == 5:
            required = 1 | 2 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 8192 | 2097152 | 4194304 | 16777216 | 33554432
            if (flags & required != required or flags & 8388608 or v[3] != 0 or
                    not v[13] or v[13] != v[14] or not v[21] or v[21] != v[22]):
                raise ValueError('inconsistent paging DMA observation')
        found, process, root = bool(flags & 1), bool(flags & 2), bool(flags & 8)
        return {'event': 'ContextEvidence', **metadata, 'operation': CONTEXT_OPERATIONS[operation],
                'flags': f'0x{flags:08x}', 'state': [name for bit, name in enumerate(CONTEXT_FLAGS) if flags & (1 << bit)],
                'status': f'0x{v[3] & 0xffffffff:08x}', 'engine_identity': f'0x{v[4]:016x}',
                'context': f'0x{v[5]:016x}', 'device': f'0x{v[6]:016x}' if found else None,
                'process': f'0x{v[7]:016x}' if process else None,
                'adapter_luid': f'{v[9]:08x}{v[8]:08x}' if flags & 4 else None,
                'context_id': v[10] if found else None, 'context_flags': f'0x{v[11]:08x}' if found else None,
                'process_flags': f'0x{v[12]:08x}' if process else None,
                'engine_generation': v[13], 'context_generation': v[14] if found else None,
                'prior_generation': v[15] if found else None,
                'canonical_root_status': f'0x{v[16] & 0xffffffff:08x}' if found else None,
                'canonical_root_entries': v[17] if root else None,
                'canonical_root_segment_offset': v[18] if root else None,
                'canonical_root_segment': v[19] if root else None,
                'submission_fence': v[20], 'expected': v[21], 'observed': v[22],
                'decision': CONTEXT_DECISIONS[decision]}
    if event_id == 8:
        if version != 1 or size != TDR_PAYLOAD.size:
            raise ValueError('unsupported TDR contract schema')
        v = TDR_PAYLOAD.unpack(payload)
        operation, phase, flags, status = v[1], v[2], v[3], v[7]
        if v[0] != 1 or not 1 <= operation <= 5 or phase not in (0, 1) or flags & ~8191:
            raise ValueError('invalid TDR contract header')
        result_valid, snapshot = bool(flags & 2), bool(flags & 4)
        if ((result_valid and (phase != 1 or operation not in (3, 4, 5) or status != 0)) or
                (operation in (3, 4, 5) and (phase != 1 or (status == 0) != result_valid)) or
                (operation == 3 and result_valid and v[12] not in (0, 1)) or
                (flags & (1 | 4096) and not snapshot) or
                (flags & 8 and (not snapshot or operation != 3))):
            raise ValueError('inconsistent TDR contract field validity')
        queue = None
        if snapshot and operation in (3, 4):
            queue = dict(zip(('healthy', 'idle', 'paused', 'failed', 'servicing', 'pending'),
                             (bool(flags & bit) for bit in (16, 32, 256, 512, 1024, 2048))))
            queue['failure_status'] = f'0x{v[14] & 0xffffffff:08x}'
        return {'event': 'TdrContract', **metadata, 'operation': TDR_OPERATIONS[operation],
                'phase': 'return' if phase else 'entry', 'engine_identity': f'0x{v[4]:016x}',
                'adapter_luid': f'{v[6]:08x}{v[5]:08x}' if flags & 4096 else None,
                'node': v[8], 'engine': v[9], 'status': f'0x{status & 0xffffffff:08x}' if phase else None,
                'flags': f'0x{flags:08x}', 'engine_snapshot': snapshot,
                'last_submitted_fence': v[10] if flags & 1 else None,
                'last_completed_fence': v[11] if snapshot else None,
                'result': v[12] if result_valid else None,
                'previous_query_fence': v[13] if flags & 8 else None,
                'queue': queue, 'recovery_pending': bool(flags & 64) if snapshot else None,
                'resetting': bool(flags & 128) if snapshot else None}
    if event_id == 4:
        if version != 1 or size != DISPLAY_PAYLOAD.size:
            raise ValueError('unsupported display-path schema')
        v = DISPLAY_PAYLOAD.unpack(payload)
        if v[0] != 1 or not 1 <= v[1] <= 4 or v[5] > 1 or v[10] > 1 or v[11] > 1:
            raise ValueError('invalid display-path event')
        return ({'event': 'DisplayPath', **metadata,
            'operation': DISPLAY_OPERATIONS[v[1]], 'phase': 'return' if v[5] else 'entry',
            'adapter_luid': f'{v[3]:08x}{v[2]:08x}', 'status': f'0x{v[4]:08x}',
            'path_id': v[6], 'requested_rotation': v[7], 'before_rotation': v[8],
            'after_rotation': v[9], 'source_visible': bool(v[10]), 'target_active': bool(v[11]),
            'flags': f'0x{v[12]:08x}'})
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
        return ({'event': 'ResetPacket', **metadata,
            'adapter_luid': f'{v[3]:08x}{v[2]:08x}', 'context': f'0x{v[4]:016x}',
            'dma_address': f'0x{v[5]:016x}', 'submission_fence': v[6],
            'graphics_generation': v[7], 'expected': v[8], 'dma_bytes': dma_bytes,
            'command_bytes': command_bytes, 'offset': chunk_offset, 'chunk_bytes': chunk_bytes,
            'hardware': dict(zip(RESET_HARDWARE_FIELDS, v[12:])),
            'commands_hex': payload[RESET_PACKET_HEADER.size:].hex()})
    if (event_id, version, size) == (2, 1, EXECUTION_PAYLOAD.size):
        values = EXECUTION_PAYLOAD.unpack(payload)
        if values[0] != 1:
            raise ValueError('unsupported execution payload version')
        source, stage = values[1], values[7]
        record = {'event': 'ExecutionState', **metadata,
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
        return record
    if (event_id, version, size) != (1, 1, PAYLOAD.size):
        raise ValueError('unsupported KMD recovery event schema')
    values = PAYLOAD.unpack(payload)
    if values[0] != 1:
        raise ValueError('unsupported recovery payload version')
    source = values[1]
    return ({
        'event': 'FirstRecoveryRequest', **metadata, 'source': source,
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
        records.append(_decode_event(event_id, version, payload,
            {'time': time, 'pid': pid, 'tid': tid, 'cpu': cpu}))
    if total != expected:
        raise ValueError(f'export event count differs: {total}/{expected}')
    return records, {'export_records': total,
                     'recovery_records': sum(r['event'] == 'FirstRecoveryRequest' for r in records),
                     'execution_records': sum(r['event'] == 'ExecutionState' for r in records),
                     'reset_packet_chunks': sum(r['event'] == 'ResetPacket' for r in records),
                     'display_records': sum(r['event'] == 'DisplayPath' for r in records),
                     'context_records': sum(r['event'] == 'ContextEvidence' for r in records),
                     'last_timestamp': last, 'requested_seconds': seconds,
                     'sha256': hashlib.sha256(data).hexdigest()}


def _uint(record, key, bits):
    value = record[key]
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f'invalid {key}')
    return value


def observed_display_calls(records):
    """Pair recorded boundaries only; ETW loss can hide either boundary."""
    pending, calls = {}, []
    identity = ('adapter_luid', 'pid', 'tid', 'operation', 'path_id',
                'requested_rotation', 'before_rotation', 'flags')
    for r in sorted((r for r in records if r['event'] == 'DisplayPath'),
                    key=lambda r: (r['qpc_stamp'], r['input_line'])):
        key = tuple(r[k] for k in identity)
        stack = pending.setdefault(key, [])
        boundary = {'line': r['input_line'], 'qpc_stamp': r['qpc_stamp'],
                    'status': r['status']}
        if r['phase'] == 'entry':
            call = {k: r[k] for k in identity}
            call.update(entry=boundary, returned=None,
                        observation='entry_without_recorded_return')
            if stack:
                for other in stack:
                    other['ambiguous'] = True
                call['ambiguous'] = True
            stack.append(call)
            calls.append(call)
        elif stack:
            call = stack.pop()
            call.update(returned=boundary, observation='both_boundaries_recorded')
        else:
            call = {k: r[k] for k in identity}
            call.update(entry=None, returned=boundary,
                        observation='return_without_recorded_entry')
            calls.append(call)
    return calls


def decode_live(data):
    """Keep a valid prefix and explicit gaps when a reboot cuts off JSONL."""
    records, states, issues = [], [], []
    end = data.rfind(b'\n') + 1
    lines = data[:end].split(b'\n')[:-1]
    starts, finishes = [], []
    for number, line in enumerate(lines, 1):
        try:
            item = json.loads(line)
            if not isinstance(item, dict):
                raise ValueError('record is not an object')
            if item.get('event') in ('TraceStart', 'TraceState', 'TraceEnd'):
                state = dict(item, input_line=number)
                states.append(state)
                if item['event'] == 'TraceStart':
                    starts.append(state)
                    if (item.get('clock') != 'qpc' or item.get('clock_context') != 1
                            or item.get('query_status') != 0
                            or not _uint(item, 'qpc_frequency', 64)):
                        raise ValueError('live trace has no verified QPC clock')
                elif item['event'] == 'TraceEnd':
                    finishes.append(state)
                continue
            if item.get('provider') != str(uuid.UUID(bytes_le=PROVIDER)):
                continue
            if item.get('raw') is not True:
                raise ValueError('KMD record has no raw payload')
            size = _uint(item, 'bytes', 16)
            encoded = item['data']
            if not isinstance(encoded, str) or len(encoded) != size * 2:
                raise ValueError('raw payload length mismatch')
            payload = bytes.fromhex(encoded)
            if len(payload) != size:
                raise ValueError('raw payload length mismatch')
            metadata = {'input_line': number, 'qpc_stamp': _uint(item, 'qpc_stamp', 64),
                        'pid': _uint(item, 'pid', 32), 'tid': _uint(item, 'tid', 32),
                        'header_flags': _uint(item, 'header_flags', 16)}
            # This exporter has no CPU index. Do not invent one, or label QPC
            # as FILETIME merely to pass through the old binary container.
            records.append(_decode_event(_uint(item, 'id', 16),
                _uint(item, 'version', 8), payload, metadata))
        except (ValueError, KeyError, TypeError) as error:
            issues.append({'line': number, 'error': str(error)})
    if len(starts) != 1 or len(finishes) > 1:
        issues.append({'error': 'expected one trace start and at most one trace end'})
    identity_valid = len(starts) == 1 and len(finishes) <= 1
    if starts:
        for state in states:
            if any(state.get(k) != starts[0].get(k)
                   for k in ('session', 'pid', 'qpc_frequency')):
                identity_valid = False
                issues.append({'line': state['input_line'], 'error': 'trace identity changed'})
    clock_valid = (identity_valid and starts[0].get('clock') == 'qpc'
                   and starts[0].get('clock_context') == 1
                   and starts[0].get('query_status') == 0
                   and type(starts[0].get('qpc_frequency')) is int
                   and 0 < starts[0]['qpc_frequency'] < (1 << 64))
    # A stopped session commonly returns 4201 to the final query. Its zeroed
    # counters are not a successful final loss query. Keep the last actual
    # query and the TraceEnd separately, even when ProcessTrace returned 0.
    queried = [s for s in states if s.get('query_status') == 0]
    return records, {
        'input_format': 'live-jsonl', 'clock': 'qpc' if clock_valid else 'unverified',
        'sha256': hashlib.sha256(data).hexdigest(), 'input_bytes': len(data),
        'complete_lines': len(lines), 'truncated_tail_bytes': len(data) - end,
        'issues': issues, 'kmd_records': len(records),
        'trace_start': starts[0] if len(starts) == 1 else None,
        'trace_end': finishes[0] if len(finishes) == 1 else None,
        'last_successful_trace_query': queried[-1] if queried else None,
        'loss_observations': [{k: s.get(k) for k in (
            'input_line', 'qpc_stamp', 'invalid', 'events_lost',
            'log_buffers_lost', 'realtime_buffers_lost')} for s in queried],
        'display_calls': observed_display_calls(records) if clock_valid else [],
        'display_pairing_skipped': not clock_valid,
        'coverage_proven': False,
        'coverage_note': 'Recorded boundaries only. Missing returns, zero observed loss, '
                         'and normal ProcessTrace exit do not prove fault coverage.',
    }


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
            clock = 'qpc_stamp' if 'qpc_stamp' in r else 'time'
            g.update({clock: r[clock]}, hardware=r['hardware'], chunks={}, consistent=True)
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
    parser.add_argument('input', type=Path)
    parser.add_argument('--live-jsonl', action='store_true', help='read --follow-all JSONL with QPC timestamps')
    parser.add_argument('--packet-dir', type=Path, help='write only complete reset command captures')
    args = parser.parse_args()
    records, summary = (decode_live if args.live_jsonl else decode)(args.input.read_bytes())
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
    if args.live_jsonl and (summary['issues'] or summary['truncated_tail_bytes']):
        raise SystemExit(2)
