# SPDX-License-Identifier: MIT
"""Persist a bounded live Windows ETW stream from an already authenticated child."""
from datetime import datetime, timezone
from pathlib import Path
import json
import os
import subprocess
import threading
import time

EXPECTED = frozenset({('DXGI',42), ('DxgKrnl',166), ('DxgKrnl',184), ('DXGI',43)})
FOCUSED_BLT = frozenset({('DxgKrnl',286), ('DxgKrnl',166)})

class ReceiptPipe:
    def __init__(self, command, directory, *, expected_events=EXPECTED):
        self.expected_events = frozenset(expected_events)
        if self.expected_events not in (EXPECTED, FOCUSED_BLT):
            raise ValueError('Unsupported collector event filter')
        self.directory = Path(directory)
        self.ready = threading.Event()
        self.finished = threading.Event()
        self.failure = None
        self.log = (self.directory/'wire.jsonl').open('xb')
        try:
            self.stderr = (self.directory/'ssh.stderr').open('xb')
        except BaseException:
            self.log.close()
            raise
        try:
            self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=self.stderr, bufsize=0)
        except BaseException:
            self.log.close()
            self.stderr.close()
            raise
        self.worker = threading.Thread(target=self.receive, name='present-receipts')
        self.worker.start()

    def persist(self, fields):
        record = dict(fields, host_monotonic_ns=time.monotonic_ns(),
            host_utc=datetime.now(timezone.utc).isoformat())
        self.log.write((json.dumps(record,separators=(',',':'))+'\n').encode())
        self.log.flush()
        os.fsync(self.log.fileno())
        return record['host_monotonic_ns']

    def send(self, command):
        raw = (command+'\n').encode('ascii')
        sent = self.process.stdin.write(raw)
        if sent != len(raw):
            raise OSError('Incomplete collector command')
        self.process.stdin.flush()

    def receive(self):
        event_keys = {}
        acknowledgments = {}
        requested_drain = False
        final_sequence = None
        done = False
        end = False
        try:
            while raw := self.process.stdout.readline(4097):
                if len(raw)>4096 or not raw.endswith(b'\n'):
                    raise ValueError('Collector record has invalid size or delimiter')
                line = raw.decode('ascii').strip()
                words = line.split()
                self.persist({'wire':line})
                if words[:1]==['READY'] and len(words)==4 and not self.ready.is_set():
                    if min(map(int,words[1:]))<=0:raise ValueError('Invalid ready clock')
                    self.ready.set()
                elif words[:1]==['EVENT'] and len(words)==11 and self.ready.is_set():
                    n=int(words[1]);key=(words[2],int(words[3]));payload=bytes.fromhex(words[10])
                    if n != len(event_keys)+1 or key not in self.expected_events or not 0<len(payload)==int(words[9])<=512:
                        raise ValueError('Unexpected sequence, payload or provider')
                    if n>48 or final_sequence is not None:raise ValueError('Collector delivery bound exceeded')
                    event_keys[n]=key
                    self.persist({'event_fsynced':n})
                    self.send('ACK '+str(n))
                elif words[:1]==['RECEIVED'] and len(words)==4:
                    n=int(words[1]);qpc=int(words[2]);state=int(words[3])
                    if n != len(acknowledgments)+1 or n not in event_keys or qpc<=0 or state not in (0,258):
                        raise ValueError('Invalid Windows acknowledgement')
                    acknowledgments[n]=qpc
                    if not requested_drain and {event_keys[k] for k in acknowledgments}==self.expected_events:
                        self.send('DRAIN')
                        requested_drain=True
                elif words[:1]==['DRAINED'] and len(words)==2:
                    count=int(words[1])
                    if not requested_drain or final_sequence is not None or count!=len(event_keys):
                        raise ValueError('Invalid delivery boundary')
                    final_sequence=count
                elif words[:1]==['END'] and len(words)==9:
                    values=list(map(int,words[1:]))
                    if any(values[:6]) or values[-2:]!=[len(event_keys),len(acknowledgments)] or not done:
                        raise RuntimeError('Collector reported incomplete delivery: '+line)
                    end=True
                else:
                    raise ValueError('Unexpected collector record: '+line[:120])
                if final_sequence is not None and len(acknowledgments)==final_sequence and not done:
                    self.send('DONE')
                    done=True
            if not end:
                raise EOFError('Live collector did not send a successful END')
        except BaseException as error:
            self.failure = type(error).__name__+' — '+str(error)
            try:
                self.persist({'receiver_error':self.failure})
                if not done: self.send('ABORT')
            except (OSError,ValueError):
                pass
        finally:
            try:
                for handle in (self.process.stdin,self.process.stdout,self.log,self.stderr):
                    try:handle.close()
                    except OSError as error:self.failure=self.failure or str(error)
            finally:
                self.finished.set()

    def await_ready(self, timeout):
        deadline=time.monotonic()+timeout
        while not self.ready.wait(0.1):
            if self.finished.is_set() or time.monotonic()>=deadline:
                return False
        return not self.finished.is_set()

    def settle(self, timeout):
        timed_out = not self.finished.wait(timeout)
        try:
            if timed_out:
                self.process.terminate()
            self.process.wait(timeout=5 if timed_out else 10)
        except subprocess.TimeoutExpired:
            timed_out = True
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        finally:
            self.worker.join(timeout=5)
        if self.worker.is_alive() or timed_out or self.failure is not None or self.process.returncode:
            reason = self.failure or ('Collector deadline' if timed_out else 'Collector exit '+str(self.process.returncode))
            raise RuntimeError(reason)
