# SPDX-License-Identifier: MIT
"""Receive the bounded Windows attempt stream over an authenticated child pipe."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from selectors import DefaultSelector, EVENT_READ
import subprocess
import time

from dma_attempt_capture import Capture
from dma_attempt_journal import Journal
from recover_dma_receipts import END_FIELDS, MAX_EVENTS, decode_event


def receive(command, output, metadata, *, fixture_filter=False, timeout=40):
    output = Path(output)
    with (output/"wire.jsonl").open("xb") as raw, (output/"collector.stderr").open("xb") as stderr:
        with Journal(output/"attempts.journal") as journal:
            with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=stderr, bufsize=0) as process:
                try:
                    return consume(process, raw, journal, metadata, fixture_filter, timeout)
                except BaseException:
                    process.terminate()
                    try:
                        process.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=4)
                    raise


def consume(process, raw, journal, metadata, fixture_filter, timeout):
    def send(command):
        line = (command+"\n").encode("ascii")
        if process.stdin.write(line) != len(line):
            raise OSError("short receipt command")

    sent, acknowledged = 0, 0
    pending = {}
    drained = None
    capture = None
    terminal = None
    deadline = time.monotonic()+timeout
    unread = b""
    with DefaultSelector() as watcher:
        watcher.register(process.stdout, EVENT_READ)
        while terminal is None:
            remaining = deadline-time.monotonic()
            if remaining <= 0 or not watcher.select(remaining):
                raise TimeoutError("DMA collector deadline")
            chunk = os.read(process.stdout.fileno(),4096)
            if not chunk:
                raise EOFError("collector disconnected before END")
            unread += chunk
            while b"\n" in unread:
                line, unread = unread.split(b"\n",1)
                if len(line)>2048 or terminal is not None:
                    raise ValueError("invalid collector record size or tail")
                words = line.decode("ascii").split()
                stamp = time.monotonic_ns()
                utc = datetime.now(timezone.utc).isoformat()
                raw.write((json.dumps({"kind":"wire","payload":words,"utc":utc,"host_monotonic_ns":stamp})+"\n").encode())
                raw.flush()
                os.fsync(raw.fileno())
                if words[:1]==["READY"] and len(words)==4 and capture is None:
                    pid, hz, qpc = map(int,words[1:])
                    if min(pid,hz,qpc) <= 0:
                        raise ValueError("invalid Windows clock")
                    meta = dict(metadata,collector_pid=pid,qpc_hz=hz,collector_ready_qpc=qpc)
                    capture = Capture(journal,meta,fixture_filter=fixture_filter)
                    journal.commit({"kind":"capture_ready","capture":meta})
                elif words[:1]==["EVENT"] and len(words)==10 and capture and drained is None:
                    event = decode_event(words)
                    if event["sequence"]!=sent+1 or event["sequence"]>MAX_EVENTS:
                        raise ValueError("noncontiguous collector sequence")
                    sent += 1
                    pending[sent] = event["callback_qpc"]
                    capture.observe(dict(event,host_arrival_monotonic_ns=stamp,host_arrival_utc=utc))
                    confirm = {"kind":"fsynced","payload":sent,
                               "host_monotonic_ns":time.monotonic_ns(),
                               "utc":datetime.now(timezone.utc).isoformat()}
                    raw.write((json.dumps(confirm)+"\n").encode())
                    raw.flush()
                    os.fsync(raw.fileno())
                    send(f"ACK {sent}")
                elif words[:1]==["PERSISTED"] and len(words)==3:
                    n,qpc=map(int,words[1:])
                    if n!=acknowledged+1 or n not in pending or qpc<pending.pop(n):
                        raise ValueError("invalid Windows acknowledgement")
                    acknowledged += 1
                elif words[:1]==["DRAINED"] and len(words)==2 and capture and drained is None:
                    drained=int(words[1])
                    if drained!=sent:
                        raise ValueError("invalid collector drain count")
                    send("CLOSE")
                elif words[:1]==["END"] and len(words)==11 and drained is not None:
                    values=list(map(int,words[1:]))
                    if min(values)<0 or values[7:9]!=[sent,acknowledged]:
                        raise ValueError("invalid collector coverage")
                    terminal=dict(zip(END_FIELDS,values))
                else:
                    raise ValueError("unexpected collector protocol message")
            if len(unread)>2048:
                raise ValueError("unterminated collector record")
        code=process.wait(timeout=5)
    clean=bool(not unread and not code and sent and sent==acknowledged and terminal["flushes"]
               and not any(terminal[name] for name in (*END_FIELDS[:6],"collector_error")))
    final={"kind":"capture_end","capture":capture.meta,"clean":clean,
           "terminal":terminal,"exit":code}
    journal.commit(final)
    return final
