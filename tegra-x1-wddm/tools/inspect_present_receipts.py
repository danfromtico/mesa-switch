#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the GM20B miniport's geometry and return receipts from ETL and live TSV.

Use --etl and/or --live from the same capture. ETL decoding needs dissect.etl.
Pass the application's journal QPC to measure the retained kernel event window.
Use --driver-window with the recorded drawing PID, TID and public call/return
QPCs to read miniport outcomes without running the application again.
"""
import argparse
import io
import json
import struct
import uuid
from pathlib import Path
from collections import defaultdict

OPERATIONS = {1: "present", 2: "virtual_submission", 3: "adapter_query", 4: "non_gdi_blt_rejected"}

def pairs(stream):
    grouped=defaultdict(list)
    for kind,row in stream:
        grouped[row[1:3]].append((kind,row))
    matches=defaultdict(set)
    for records in grouped.values():
        stack=[]
        # The reader's original order is retained for equal timestamps.
        for kind,row in sorted(records,key=lambda item:item[1][0]):
            if kind=='geometry':
                if stack and stack[-1][0][5]==row[3]:
                    stack[-1][1].append(row)
            elif row[3]==1:
                stack.append((row,[]))
            elif row[3]==2:
                if not stack or stack[-1][0][5]!=row[5]:
                    stack.clear()
                    continue
                enter,geometry=stack.pop()
                for sample in geometry:
                    matches[sample].add((enter[0],row[0],row[4]))
    return matches

def join_geometry(live,etl):
    a,b=pairs(live),pairs(etl)
    unique=set(live)|set(etl)
    ticks=defaultdict(int)
    for _,v in unique:
        ticks[v[:3]]+=1
    combined=pairs(sorted(unique,key=lambda item:item[1][0]))
    result={}
    for kind,row in unique:
        if kind!='geometry':
            continue
        proven=a[row]|b[row]
        source='both' if a[row] and b[row] else 'live' if a[row] else 'etl'
        if not proven:
            # When crossing sources, only strictly ordered, unique endpoints
            # and a unique geometry tick can establish the bracket.
            if ticks[row[:3]]==1:
                proven={q for q in combined[row] if q[0]<row[0]<q[1] and
                        ticks[(q[0],*row[1:3])]==1 and ticks[(q[1],*row[1:3])]==1}
            source='joined'
        if len(proven)==1:
            result[row]=(*next(iter(proven)),source)
    return result


def read_live(path, driver_events=None):
    events = []
    for line in path.read_text(errors="replace").splitlines():
        v = line.split()
        try:
            if len(v) == 15 and v[1] == "geometry":
                events.append(("geometry", (int(v[0]), int(v[2]), int(v[3]), *(int(n) for n in v[4:]))))
            elif len(v) == 14 and v[1] == "kmd":
                operation = int(v[4])
                row = (int(v[0]), int(v[2]), int(v[3]), int(v[5]), int(v[6], 16), int(v[7], 16))
                if v[4] == "1":
                    events.append(("edge", row))
                if driver_events is not None and operation in OPERATIONS and row[3] in (1, 2):
                    driver_events.append((*row, operation))
        except ValueError:
            continue
    return events


def read_etl(path, driver_events=None):
    from dissect.etl import ETL
    from dissect.etl.c_etl import BufferFlag, c_etl
    from dissect.etl.etl import Buffer
    from dissect.etl.headers.event import EventHeader

    data = path.read_bytes()
    etl = ETL(io.BytesIO(data))
    provider = uuid.UUID("c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0").bytes_le
    events = []
    damaged = []
    last = None
    empty = 0
    for offset in range(0, len(data), etl.buffer_size):
        try:
            block = Buffer(etl, offset)
            hdr = block.header
            if hdr.BufferSize == hdr.FilledBytes == 0:
                empty += 1
                continue
            if (hdr.BufferSize != etl.buffer_size or hdr.LoggerId != etl.buffer_header.LoggerId or
                hdr.BufferFlag & BufferFlag.COMPRESSED or
                not len(c_etl.BufferHeader) <= hdr.FilledBytes <= min(len(data) - offset, hdr.BufferSize)):
                raise ValueError("partial buffer")
            for record in block:
                event = record.header
                if not isinstance(event, EventHeader):
                    continue
                last = max(last or 0, event.time_delta)
                item = event.header
                if bytes(item.ProviderId) != provider or item.Version != 1 or item.Level != 4 or item.Keywords != 2:
                    continue
                prefix = (event.time_delta, event.process_id, event.thread_id)
                if item.Id == 5 and len(event.payload) == 44:
                    v = struct.unpack("<11I", event.payload)
                    if v[0] == v[1] == 1 and v[2] in (1, 2):
                        events.append(("edge", (*prefix, v[2], v[3], v[4])))
                    if driver_events is not None and v[0] == 1 and v[1] in OPERATIONS and v[2] in (1, 2):
                        driver_events.append((*prefix, v[2], v[3], v[4], v[1]))
                elif item.Id == 6 and len(event.payload) == 48:
                    v = struct.unpack("<12I", event.payload)
                    if v[0] == 1:
                        events.append(("geometry", (*prefix, *v[1:])))
        except (EOFError, ValueError, struct.error) as error:
            damaged.append([offset, type(error).__name__])
    h = etl.logfile_header.header
    finalized = bool(h.EndTime and h.BuffersWritten * etl.buffer_size == len(data) and
                     not (empty or damaged or h.EventsLost or h.BufferLost))
    return events, {"finalized_without_loss": finalized, "last_retained_qpc": last,
                    "frequency": int(h.PerfFreq), "damaged_buffers": damaged}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--etl", type=Path)
    parser.add_argument("--live", type=Path)
    parser.add_argument("--present-qpc", type=int)
    parser.add_argument("--driver-window", type=int, nargs=4, metavar=("PID", "TID", "CALL_QPC", "RETURN_QPC"))
    args = parser.parse_args()
    if not args.live and not args.etl:
        parser.error("supply --live or --etl")
    live_driver = etl_driver = None
    if args.driver_window:
        pid, tid, start, end = args.driver_window
        if min(pid, tid) <= 0 or start < 0 or end < start:
            parser.error("expected a positive PID/TID and an ordered QPC interval")
        live_driver, etl_driver = [], []
    live = read_live(args.live, live_driver) if args.live else []
    etl, quality = read_etl(args.etl, etl_driver) if args.etl else ([], {})
    pairs = join_geometry(live, etl)
    # Keep repeat records with the same payload and timestamp; both streams
    # can contain the same record, so take their maximum multiplicity.
    from collections import Counter
    geometries = Counter(row for tag, row in live if tag == "geometry") | Counter(row for tag, row in etl if tag == "geometry")
    result = []
    for row in sorted(geometries.elements()):
        bracket = pairs.get(row)
        result.append({"qpc": row[0], "pid": row[1], "tid": row[2], "flags": row[3],
                       "source": row[4:9], "destination": row[9:14],
                       "return_status": bracket[2] if bracket else None, "pair_source": bracket[3] if bracket else None})
    if args.present_qpc is not None and quality.get("last_retained_qpc") is not None and quality.get("frequency"):
        quality["last_etw_ms_relative_to_present_checkpoint"] = round(
            1000 * (quality["last_retained_qpc"] - args.present_qpc) / quality["frequency"], 3)
    report = {"trace": quality, "geometry": result}
    if args.driver_window:
        counts = Counter(live_driver) | Counter(etl_driver)
        report["driver_window"] = {"pid": pid, "tid": tid, "call_qpc": start, "return_qpc": end, "receipts": [
            {"qpc": qpc, "operation": OPERATIONS[op], "phase": "enter" if phase == 1 else "result",
             "status": f"0x{status:08x}", "flags": f"0x{flags:08x}"}
            for qpc, owner, thread, phase, status, flags, op in sorted(counts.elements(), key=lambda row: row[0])
            if (owner, thread) == (pid, tid) and start <= qpc <= end
        ]}
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
