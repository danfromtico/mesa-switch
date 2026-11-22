#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the independent DMA receiver's wire.jsonl, including interrupted captures.

Use the original run manifest to bind the stream to a Windows boot and capture.
Wire sequence numbers identify receipts within that stream. They are separate
from the Windows queue sequence and the scheduler's DMA submission ID.
"""

import argparse
import json
from pathlib import Path


MAX_EVENTS = 8192
MAX_LINE = 4096
KMD_SIZES = {(1, 1): 72, (2, 1): 152, (7, 1): 32, (7, 2): 32}
DXG_SIZES = {(105, 0): 4, (106, 0): 4, (175, 1): 44,
             (176, 0): 28, (177, 1): 52, (180, 1): 32}
ATTEMPT_DXG_SIZES = {(27, 2): 60, (29, 2): 60, (30, 0): 64, (31, 0): 64,
                     (32, 0): 64, (178, 1): 60, (433, 1): 32}
END_FIELDS = ("status", "stop", "process_trace", "events_lost",
              "realtime_buffers_lost", "file_buffers_lost", "flushes",
              "sent", "windows_acknowledged", "collector_error")


def decode_event(words):
    n, event, version, pid, tid, source, callback, size = map(int, words[1:9])
    payload = bytes.fromhex(words[9])
    key = (event, version)
    if size != len(payload) or pid < 0 or tid <= 0 or not 0 < source <= callback:
        raise ValueError("invalid event source or payload")
    if size == KMD_SIZES.get(key):
        provider = "TegraKMD"
    elif (size == ATTEMPT_DXG_SIZES.get(key)
          or (key == (110, 1) and 56 <= size <= 512)
          or (size == DXG_SIZES.get(key) and (pid == 4 or event in (175, 176, 177, 180)))):
        provider = "DxgKrnl"
    else:
        raise ValueError("unknown event identity")
    return {
        "sequence": n, "provider": provider, "event_id": event, "version": version,
        "pid": pid, "tid": tid, "source_qpc": source, "callback_qpc": callback,
        "payload_hex": payload.hex(),
    }


def recover(path):
    events = []
    ready = drained = terminal = receiver_error = recovery_error = None
    host_ns = acknowledged = tail_bytes = 0
    with path.open("rb") as stream:
        for line_number in range(1, 3 * MAX_EVENTS + 6):
            raw = stream.readline(MAX_LINE + 1)
            if not raw:
                break
            if len(raw) > MAX_LINE:
                recovery_error = {"line": line_number, "reason": "oversize journal record"}
                break
            if not raw.endswith(b"\n"):
                tail_bytes = len(raw)
                break
            try:
                record = json.loads(raw)
                stamp = record["host_monotonic_ns"]
                utc = record["utc"]
                kind, value = record["kind"], record["payload"]
                if type(stamp) is not int or stamp <= 0 or stamp < host_ns or not isinstance(utc, str):
                    raise ValueError("invalid receiver clock")
                if receiver_error is not None:
                    raise ValueError("data follows receiver exception")
                host_ns = stamp
                if kind == "exception" and isinstance(value, dict):
                    receiver_error = value
                    continue
                if terminal is not None:
                    raise ValueError("data follows terminal record")
                if kind == "fsynced":
                    if (not events or type(value) is not int or value != len(events)
                            or "host_after_fsync_ns" in events[-1]):
                        raise ValueError("invalid fsync confirmation")
                    events[-1].update(host_after_fsync_ns=stamp, host_after_fsync_utc=utc)
                    continue
                if (kind != "wire" or not isinstance(value, list) or not value
                        or not all(isinstance(word, str) for word in value)):
                    raise ValueError("invalid wire record")
                if events and "host_after_fsync_ns" not in events[-1]:
                    raise ValueError("wire data follows an event without an fsync confirmation")
                name = value[0]
                if name == "READY" and len(value) == 4 and ready is None:
                    pid, frequency, qpc = map(int, value[1:])
                    if min(pid, frequency, qpc) <= 0:
                        raise ValueError("invalid READY")
                    ready = {"collector_pid": pid, "qpc_hz": frequency, "ready_qpc": qpc}
                elif name == "EVENT" and len(value) == 10 and ready and drained is None:
                    event = decode_event(value)
                    if event["sequence"] != len(events) + 1 or len(events) == MAX_EVENTS:
                        raise ValueError("invalid event sequence")
                    events.append(dict(event, host_arrival_ns=stamp, host_arrival_utc=utc))
                elif name == "PERSISTED" and len(value) == 3:
                    n, qpc = map(int, value[1:])
                    if n != acknowledged + 1 or n > len(events) or qpc < events[n - 1]["callback_qpc"]:
                        raise ValueError("invalid Windows acknowledgement")
                    events[n - 1]["windows_ack_qpc"] = qpc
                    acknowledged += 1
                elif name == "DRAINED" and len(value) == 2 and ready and drained is None:
                    drained = int(value[1])
                    if drained != len(events):
                        raise ValueError("invalid drain count")
                elif name == "END" and len(value) == 11 and drained is not None:
                    numbers = list(map(int, value[1:]))
                    if min(numbers) < 0 or numbers[7:9] != [drained, acknowledged]:
                        raise ValueError("invalid terminal counts")
                    terminal = dict(zip(END_FIELDS, numbers))
                else:
                    raise ValueError("unexpected wire message")
            except (ValueError, KeyError, IndexError, AttributeError, TypeError) as exc:
                recovery_error = {"line": line_number, "reason": str(exc)}
                break
        else:
            recovery_error = {"line": line_number, "reason": "journal exceeds the maximum receipt count"}

    return {
        "ready": ready, "drained": drained, "terminal": terminal,
        "receiver_error": receiver_error, "recovery_error": recovery_error,
        "incomplete_tail_bytes": tail_bytes,
        "counts": {"arrived_and_recovered": len(events),
                   "confirmed_fsync": sum("host_after_fsync_ns" in row for row in events),
                   "windows_acknowledged": acknowledged},
        "capture_end_verified": bool(terminal and receiver_error is None and recovery_error is None
                                      and not tail_bytes
                                      and drained and drained == acknowledged and terminal["flushes"]
                                      and not any(terminal[field] for field in
                                                  (*END_FIELDS[:6], "collector_error"))),
        "events": events,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    args = parser.parse_args()
    result = recover(args.journal)
    print(json.dumps(result, indent=2))
    if result["recovery_error"] is not None:
        raise SystemExit(1)
