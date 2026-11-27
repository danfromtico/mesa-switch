# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    'decode_recovery', Path(__file__).resolve().parents[1] / 'tools/decode_recovery.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)

# Fixed wire bytes, independent of the decoder's field packing. Match the
# 72-byte version-1 C record, including a negative NTSTATUS and 64-bit handles.
PAYLOAD = bytes.fromhex('''
    01000000 04000000 88776655 44332211 170000c0 09000000
    8070605040302010 40000000 00010000 20000000 6c000000
    6b000000 03000000 01000000 02000000 04000000 08000000
''')


def export(payload=PAYLOAD, version=1, count=1, event_id=1):
    return (struct.pack('<8sQQII', b'SWETW001', count, 1337, 12, 48) +
            struct.pack('<Q16sIIHHHHBBB5x', 1337, decoder.PROVIDER,
                        4, 6020, 2, event_id, 0x40, len(payload), 0, version, 2) + payload)


START = {'event': 'TraceStart', 'session': 'capture', 'pid': 1200,
         'clock': 'qpc', 'clock_context': 1, 'qpc_stamp': 500,
         'qpc_frequency': 19200000, 'query_status': 0, 'process_status': 0,
         'events_lost': 0, 'log_buffers_lost': 0, 'realtime_buffers_lost': 0,
         'invalid': 0}


def live_event(payload=PAYLOAD, event_id=1, **changes):
    record = {'provider': 'c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0',
              'raw': True, 'id': event_id, 'version': 1, 'header_flags': 576,
              'pid': 4, 'tid': 6020, 'qpc_stamp': 1337,
              'bytes': len(payload), 'data': payload.hex()}
    record.update(changes)
    return record


def json_lines(*records):
    return b''.join(json.dumps(r).encode() + b'\n' for r in records)


class LiveRecoveryDecode(unittest.TestCase):
    def test_qpc_and_original_payload_without_invented_metadata(self):
        records, summary = decoder.decode_live(json_lines(START, live_event()))
        binary, _ = decoder.decode(export())
        expected = {k: v for k, v in binary[0].items() if k not in ('time', 'cpu')}
        expected.update(input_line=2, qpc_stamp=1337, header_flags=576)
        self.assertEqual(records, [expected])
        self.assertEqual(summary['issues'], [])
        self.assertEqual(summary['clock'], 'qpc')
        self.assertFalse(summary['coverage_proven'])
        self.assertIsNone(summary['trace_end'])

    def test_reboot_tail_preserves_prefix_and_prior_loss(self):
        queried = dict(START, event='TraceState', events_lost=3)
        stopped = dict(START, event='TraceEnd', query_status=4201, clock_context=0)
        raw = json_lines(START, live_event(), queried, stopped) + b'{"provider":'
        records, summary = decoder.decode_live(raw)
        self.assertEqual(len(records), 1)
        self.assertEqual(summary['truncated_tail_bytes'], 12)
        self.assertEqual(summary['last_successful_trace_query']['events_lost'], 3)
        self.assertEqual(summary['trace_end']['query_status'], 4201)
        self.assertFalse(summary['coverage_proven'])

    def test_corruption_and_unknown_schema_remain_explicit(self):
        for change in ({'bytes': 71}, {'data': 'zz' * 72}, {'version': 2},
                       {'qpc_stamp': -1}, {'pid': True}, {'raw': False}):
            with self.subTest(change=change):
                records, summary = decoder.decode_live(json_lines(START, live_event(**change)))
                self.assertEqual(records, [])
                self.assertEqual(len(summary['issues']), 1)
        records, summary = decoder.decode_live(json_lines(START) + b'\x00\x00\n' + json_lines(live_event()))
        self.assertEqual(len(records), 1)
        self.assertEqual(summary['issues'][0]['line'], 2)

    def test_display_boundaries_keep_threads_and_missing_calls_separate(self):
        entry = bytes.fromhex('''
            01000000 03000000 88776655 44332211 03010000 00000000
            00000000 02000000 01000000 01000000 01000000 01000000 01000000
        ''')
        returned = bytearray(entry)
        struct.pack_into('<II', returned, 16, 0, 1)
        struct.pack_into('<I', returned, 36, 2)
        events = [live_event(entry, 4, qpc_stamp=1000),
                  live_event(returned, 4, tid=42, qpc_stamp=1500),
                  live_event(returned, 4, qpc_stamp=2000),
                  live_event(entry, 4, qpc_stamp=3000)]
        _, summary = decoder.decode_live(json_lines(START, *events))
        self.assertEqual([c['observation'] for c in summary['display_calls']], [
            'both_boundaries_recorded', 'return_without_recorded_entry',
            'entry_without_recorded_return'])
        self.assertEqual(summary['display_calls'][0]['returned']['qpc_stamp'], 2000)
        self.assertFalse(summary['coverage_proven'])
        # A different consumer/boot cannot be paired with the first stream.
        _, mixed = decoder.decode_live(json_lines(START, *events, dict(START, pid=99)))
        self.assertTrue(mixed['display_pairing_skipped'])
        self.assertEqual(mixed['display_calls'], [])

    def test_unverified_clock_and_other_providers(self):
        for start in (dict(START, clock_context=2), dict(START, qpc_frequency=0)):
            _, summary = decoder.decode_live(json_lines(start, live_event()))
            self.assertEqual(summary['clock'], 'unverified')
            self.assertTrue(summary['issues'])
        records, summary = decoder.decode_live(json_lines(START, live_event(provider='other')))
        self.assertEqual(records, [])
        self.assertEqual(summary['issues'], [])


class RecoveryDecode(unittest.TestCase):
    def test_display_path_entry_and_return(self):
        payload = bytes.fromhex('''
            01000000 03000000 88776655 44332211 00000000 01000000
            00000000 02000000 01000000 02000000 01000000 01000000 01000000
        ''')
        records, summary = decoder.decode(export(payload, event_id=4))
        r = records[0]
        self.assertEqual(summary['display_records'], 1)
        self.assertEqual((r['operation'], r['phase'], r['status']),
                         ('update_active_path', 'return', '0x00000000'))
        self.assertEqual(r['adapter_luid'], '1122334455667788')
        self.assertEqual((r['requested_rotation'], r['before_rotation'], r['after_rotation']), (2, 1, 2))
        self.assertTrue(r['source_visible'] and r['target_active'])
        entry = bytearray(payload)
        struct.pack_into('<II', entry, 16, 0x103, 0)
        self.assertEqual(decoder.decode(export(entry, event_id=4))[0][0]['phase'], 'entry')
        for bad in (payload[:-4], b'\x02' + payload[1:]):
            with self.assertRaises(ValueError): decoder.decode(export(bad, event_id=4))

    def test_reset_packet_chunks_and_missing_data(self):
        # 144-byte header, little-endian identities, then actual command words.
        header = bytes.fromhex('''
            01000000 08000000 88776655 44332211
            8070605040302010 0040000400000000
            ed020000 01000000 50000000 10000000 10000000 00000000
        ''') + struct.pack('<22I', 15, *range(1, 22))
        records, summary = decoder.decode(export(header + b'abcdefgh', event_id=3))
        self.assertEqual(summary['reset_packet_chunks'], 1)
        r = records[0]
        self.assertEqual(r['context'], '0x1020304050607080')
        self.assertEqual(r['dma_address'], '0x0000000004004000')
        self.assertEqual((r['submission_fence'], r['expected'], r['command_bytes']), (749, 80, 16))
        self.assertEqual(r['hardware']['pb_get_lo'], 11)
        self.assertFalse(decoder.reconstruct_reset_packets(records)[0]['complete'])
        last = dict(r, offset=8, commands_hex=b'ijklmnop'.hex())
        complete = decoder.reconstruct_reset_packets([r, last])[0]
        self.assertEqual(complete['data'], b'abcdefghijklmnop')
        self.assertTrue(complete['complete'])
        self.assertFalse(decoder.reconstruct_reset_packets([last])[0]['complete'])
        self.assertFalse(decoder.reconstruct_reset_packets([r, last, last])[0]['complete'])
        changed = dict(last, hardware=dict(last['hardware'], gp_get=42))
        self.assertFalse(decoder.reconstruct_reset_packets([r, changed])[0]['complete'])
        for bad in (header + b'abcd', b'\x02' + header[1:] + b'abcdefgh'):
            with self.assertRaises(ValueError):
                decoder.decode(export(bad, event_id=3))

    def test_execution_wire_identity(self):
        payload = bytes.fromhex('''
            01000000 01000000 88776655 44332211 170000c0 3e000000 09000000 07000000
            8070605040302010 8877665544332211 0010000001000000
            e4020000 e3020000 80000000 7f000000 800c0000 21810200 19010000 a30000c0
            02000000 03000000 34260000 00001000 01000000 02000000 04000000 08000000
            00100000 00010000 00020000 00040000 14000000 800c0000 00020000 20000000
        ''')
        records, summary = decoder.decode(export(payload, event_id=2))
        self.assertEqual((summary['recovery_records'], summary['execution_records']), (0, 1))
        r = records[0]
        self.assertEqual(r['event'], 'ExecutionState')
        self.assertEqual((r['source_name'], r['stage_name']), ('preempt', 'gpu_fence'))
        self.assertEqual(r['request_context'], '0x1020304050607080')
        self.assertEqual(r['context'], '0x1122334455667788')
        self.assertEqual(r['dma_address'], '0x0000000100001000')
        self.assertEqual([r[k] for k in ('request_fence', 'submission_fence', 'last_completed_fence',
                         'expected', 'observed', 'dma_bytes', 'graphics_generation', 'poll_count')],
                         [62, 740, 739, 128, 127, 3200, 2, 20])
        self.assertEqual(r['queue_failure'], '0xc00000a3')
        self.assertEqual(r['work_flags'], '0x00028121')
        self.assertEqual(r['work_state'], ['active', 'prepared', 'submitted', 'observed_valid', 'mc_valid'])
        self.assertEqual(r['queue_state'], ['engine', 'preemptions_pending', 'pending', 'channel_fields_valid'])
        self.assertEqual(r['channel_register'], '0x00002634')
        self.assertEqual(r['cached_fecs_host'], '0x00000400')
        self.assertEqual((r['request_dma_bytes'], r['private_bytes'], r['umd_private_bytes']), (3200, 512, 32))
        for value in (payload[:-1], b'\x02' + payload[1:]):
            with self.assertRaises(ValueError):
                decoder.decode(export(value, event_id=2))

    def test_wire_identity(self):
        records, summary = decoder.decode(export())
        self.assertEqual(summary['export_records'], 1)
        self.assertEqual(summary['recovery_records'], 1)
        r = records[0]
        self.assertEqual((r['pid'], r['tid'], r['cpu'], r['time']), (4, 6020, 2, 1337))
        self.assertEqual(r['source_name'], 'preempt')
        self.assertEqual(r['adapter_luid'], '1122334455667788')
        self.assertEqual(r['status'], '0xc0000017')
        self.assertEqual(r['context'], '0x1020304050607080')
        self.assertEqual([r[k] for k in ('dma_bytes', 'private_bytes', 'umd_private_bytes',
                         'submission_fence', 'last_completed_fence', 'graphics_generation')],
                         [64, 256, 32, 108, 107, 3])
        self.assertEqual([r[k] for k in ('cached_fifo', 'cached_mmu', 'cached_pbdma', 'cached_pbdma1')],
                         ['0x00000001', '0x00000002', '0x00000004', '0x00000008'])

    def test_incomplete_or_incompatible_export(self):
        for data in (export()[:31], export()[:79], export()[:-1],
                     export(count=2), export(version=2), export(PAYLOAD[:-1]),
                     export(b'\x02' + PAYLOAD[1:])):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                decoder.decode(data)

    def test_other_provider_is_not_a_recovery(self):
        data = bytearray(export())
        data[40:56] = bytes(16)
        records, summary = decoder.decode(data)
        self.assertEqual(records, [])
        self.assertEqual(summary['export_records'], 1)

    def test_fault_isr_v2_and_isr_recovery_source(self):
        isr_rec = bytearray(PAYLOAD)
        struct.pack_into('<I', isr_rec, 4, 7)
        recs, summary = decoder.decode(export(isr_rec, event_id=1))
        self.assertEqual(recs[0]['source_name'], 'isr')
        self.assertEqual(summary['recovery_records'], 1)

        top = struct.pack('<26I',
            2, 0x02, 0x55667788, 0x11223344,
            1, 0x50000100, 0, 0,
            0, 0xffffffff, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 1,
            1, 1)
        cap = struct.pack('<26I',
            2, 0x3e, 0x55667788, 0x11223344,
            1, 0x50000100, 0x10000000, 0x24,
            2, 2, 0x80094000, 0xdeadbee0,
            1, 0x800000ea, 0, 0,
            8, 0x100, 1, 0,
            0x80123400, 0xabcdef, 0xbad00002, 1,
            1, 2)
        raw = (struct.pack('<8sQQII', b'SWETW001', 2, 1338, 12, 48) +
               struct.pack('<Q16sIIHHHHBBB5x', 1337, decoder.PROVIDER,
                           4, 6020, 2, 10, 0x40, len(top), 0, 2, 2) + top +
               struct.pack('<Q16sIIHHHHBBB5x', 1338, decoder.PROVIDER,
                           4, 6020, 2, 10, 0x40, len(cap), 0, 2, 2) + cap)
        f_recs, f_sum = decoder.decode(raw)
        self.assertEqual(f_sum['fault_isr_records'], 2)
        self.assertEqual(f_recs[0]['phase'], 'top_half')
        self.assertIsNone(f_recs[0]['fault_id'])
        self.assertEqual(f_recs[0]['state'], ['stall_irq'])
        self.assertEqual(f_recs[1]['phase'], 'passive_capture')
        self.assertEqual(f_recs[1]['fault_id'], 2)
        self.assertEqual(f_recs[1]['fault_addr_lo'], '0xdeadbee0')
        self.assertEqual(f_recs[1]['fault_addr_hi'], '0x00000001')
        self.assertIn('details_captured', f_recs[1]['state'])
        self.assertIn('multi_mmu', f_recs[1]['state'])
        for bad_ver, bad_payload in ((1, top), (2, top[:-4]),
                                     (2, struct.pack('<I', 1) + top[4:]),
                                     (2, top[:4] + struct.pack('<I', 0x80) + top[8:]),
                                     (2, top[:-4] + struct.pack('<I', 3))):
            with self.assertRaises(ValueError):
                decoder.decode(export(bad_payload, version=bad_ver, event_id=10))


class ContextEvidenceDecode(unittest.TestCase):
    # Independent fixed 120-byte wire record. Named flags are producer
    # observations, not guesses derived from Windows' raw ContextFlags bits.
    FAILURE = bytes.fromhex('''
        01000000 06000000 0f2c0200 a30000c0
        2222222211111111 55555555aaaaaaaa
        3333333333333333 4444444444444444
        88776655 44332211 07000000 14000000
        01000000 02000000 03000000 03000000
        00000000 00400000 0000040000000000
        01000000 51000000 00000000 00000000
        06000000 00000000
    ''')

    def decode_one(self, data):
        return decoder._decode_event(9, 1, data, {'qpc_stamp': 10})

    def test_branch_point_identity_and_named_flags(self):
        value = self.decode_one(self.FAILURE)
        self.assertEqual(len(self.FAILURE), 120)
        self.assertEqual(value['event'], 'ContextEvidence')
        self.assertEqual(value['operation'], 'paging_capture_failure')
        self.assertEqual(value['context'], '0xaaaaaaaa55555555')
        self.assertEqual(value['context_id'], 7)
        self.assertEqual(value['adapter_luid'], '1122334455667788')
        self.assertEqual(value['decision'], 'stale_generation')
        self.assertEqual((value['engine_generation'], value['context_generation']), (2, 3))
        self.assertEqual(value['submission_fence'], 81)
        self.assertIn('system_context', value['state'])
        self.assertIn('system_process', value['state'])
        self.assertEqual(value['canonical_root_segment_offset'], 0x40000)

    def test_same_pointer_does_not_replace_creation_serial(self):
        first, second = bytearray(self.FAILURE), bytearray(self.FAILURE)
        struct.pack_into('<I', second, 56, 8)
        a, b = self.decode_one(first), self.decode_one(second)
        self.assertEqual(a['context'], b['context'])
        self.assertNotEqual(a['context_id'], b['context_id'])

    def test_absent_object_never_supplies_creation_flags(self):
        raw = bytearray(self.FAILURE)
        struct.pack_into('<I', raw, 8, 0)
        struct.pack_into('<I', raw, 112, 2)
        value = self.decode_one(raw)
        self.assertIsNone(value['context_id'])
        self.assertIsNone(value['context_flags'])
        self.assertIsNone(value['process_flags'])
        self.assertIsNone(value['canonical_root_status'])

    def test_invalid_root_never_supplies_old_binding(self):
        raw = bytearray(self.FAILURE)
        flags = struct.unpack_from('<I', raw, 8)[0]
        struct.pack_into('<I', raw, 8, flags & ~8)
        value = self.decode_one(raw)
        self.assertEqual(value['context_id'], 7)
        self.assertEqual(value['canonical_root_status'], '0x00000000')
        for key in ('canonical_root_entries', 'canonical_root_segment_offset', 'canonical_root_segment'):
            self.assertIsNone(value[key])

    def test_both_export_formats_preserve_context_record(self):
        raw = bytearray(self.FAILURE)
        struct.pack_into('<I', raw, 52, 0x80000001)  # Negative LUID.HighPart.
        binary, summary = decoder.decode(export(raw, event_id=9))
        live, live_summary = decoder.decode_live(json_lines(START, live_event(raw, 9)))
        self.assertEqual(summary['context_records'], 1)
        self.assertEqual(live_summary['issues'], [])
        self.assertEqual(binary[0]['adapter_luid'], '8000000155667788')
        self.assertEqual(live[0]['adapter_luid'], binary[0]['adapter_luid'])
        omit = ('time', 'cpu', 'qpc_stamp', 'input_line', 'header_flags')
        self.assertEqual({k: v for k, v in live[0].items() if k not in omit},
                         {k: v for k, v in binary[0].items() if k not in omit})
        self.assertFalse(live_summary['coverage_proven'])

    def test_work_and_operation_validity_flags(self):
        flags = struct.unpack_from('<I', self.FAILURE, 8)[0]
        work_only = (32, 64, 128, 256, 512, 524288, 1048576, 4194304, 8388608, 16777216)
        for operation in (1, 2, 3, 4, 6):
            for flag in work_only:
                raw = bytearray(self.FAILURE)
                struct.pack_into('<I', raw, 4, operation)
                struct.pack_into('<I', raw, 8, flags | flag)
                if operation != 6:
                    struct.pack_into('<I', raw, 112, 0)
                with self.subTest(operation=operation, flag=flag), self.assertRaises(ValueError):
                    self.decode_one(raw)
        for operation in (1, 2, 3, 4, 5):
            raw = bytearray(self.FAILURE)
            struct.pack_into('<II', raw, 4, operation, 0)
            struct.pack_into('<I', raw, 112, 0)
            with self.subTest(missing_found=operation), self.assertRaises(ValueError):
                self.decode_one(raw)
        for operation in (1, 2, 3, 6):
            for extra in (16, 2097152):
                raw = bytearray(self.FAILURE)
                struct.pack_into('<II', raw, 4, operation, flags | extra)
                if operation != 6:
                    struct.pack_into('<I', raw, 112, 0)
                with self.subTest(operation=operation, extra=extra), self.assertRaises(ValueError):
                    self.decode_one(raw)

    def test_marker_observation_requires_all_source_predicates(self):
        raw = bytearray(self.FAILURE)
        flags = (1 | 2 | 4 | 8 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 8192 |
                 131072 | 524288 | 1048576 | 2097152 | 4194304 | 16777216 | 33554432)
        struct.pack_into('<III', raw, 4, 5, flags, 0)
        struct.pack_into('<II', raw, 72, 2, 2)
        struct.pack_into('<III', raw, 104, 9, 9, 0)
        value = self.decode_one(raw)
        self.assertEqual(value['operation'], 'paging_dma_complete')
        self.assertIn('work_root_matches_canonical', value['state'])
        self.assertEqual(value['expected'], value['observed'])
        for missing in (1, 2, 32, 64, 128, 256, 512, 1024, 2048, 8192,
                        2097152, 4194304, 16777216, 33554432):
            changed = bytearray(raw)
            struct.pack_into('<I', changed, 8, flags & ~missing)
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                self.decode_one(changed)
        changed = bytearray(raw);struct.pack_into('<I', changed, 108, 8)
        with self.assertRaises(ValueError):self.decode_one(changed)
        for offset, bad in ((8, flags | 8388608), (12, 0xc00000a3), (68, 0),
                            (72, 0), (72, 3), (104, 0)):
            changed = bytearray(raw);struct.pack_into('<I', changed, offset, bad)
            with self.subTest(offset=offset, bad=bad), self.assertRaises(ValueError):
                self.decode_one(changed)

    def test_unknown_schema_and_reserved_bits_remain_errors(self):
        for offset, value in ((0, 2), (8, 0x80000000), (112, 0), (116, 1), (12, 0)):
            changed = bytearray(self.FAILURE);struct.pack_into('<I', changed, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.decode_one(changed)
        with self.assertRaises(ValueError):self.decode_one(self.FAILURE[:-1])


if __name__ == '__main__':
    unittest.main()
