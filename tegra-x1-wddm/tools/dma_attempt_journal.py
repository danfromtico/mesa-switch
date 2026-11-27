#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Durable, append-only checkpoints for a single Windows DMA evidence capture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import time


MAX_RECORD = 64 * 1024
STAGES = ("submit", "kmd_entry", "kmd_return", "gpu_complete",
          "driver_notification", "dma_stop", "retire")


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def framed(record):
    raw = encoded({"record": record, "sha256": digest(record)}) + b"\n"
    if len(raw) > MAX_RECORD:
        raise ValueError("checkpoint exceeds the record bound")
    return raw


def append_and_sync(fd, record):
    pending = memoryview(framed(record))
    while pending:
        size = os.write(fd, pending)
        if size <= 0:
            raise OSError("no progress writing DMA journal")
        pending = pending[size:]
    os.fsync(fd)


class Journal:
    def __init__(self, path):
        self.fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            parent = os.open(Path(path).parent, os.O_RDONLY)
            try:
                os.fsync(parent)
            finally:
                os.close(parent)
        except BaseException:
            os.close(self.fd)
            raise
        self.serial = 0
        self.poisoned = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        os.close(self.fd)

    def commit(self, snapshot):
        if self.poisoned:
            raise OSError("an interrupted write requires a new journal")
        self.poisoned = True
        serial = self.serial + 1
        record = {"serial": serial, "snapshot": snapshot}
        append_and_sync(self.fd, record)
        receipt = {"committed": serial, "record_sha256": digest(record),
                   "after_data_fsync_monotonic_ns": time.monotonic_ns()}
        append_and_sync(self.fd, receipt)
        self.serial = serial
        self.poisoned = False
        return receipt


def recover(path):
    committed = []
    waiting = None
    tail = None
    with Path(path).open("rb") as stream:
        while True:
            raw = stream.readline(MAX_RECORD + 1)
            if not raw:
                break
            try:
                if len(raw) > MAX_RECORD or not raw.endswith(b"\n"):
                    raise ValueError("incomplete or oversized frame")
                envelope = json.loads(raw)
                record = envelope["record"]
                checksum = digest(record)
                if envelope["sha256"] != checksum:
                    raise ValueError("frame checksum mismatch")
                serial = len(committed) + 1
                if waiting is None:
                    if record["serial"] != serial or not isinstance(record["snapshot"], dict):
                        raise ValueError("unexpected snapshot sequence")
                    waiting = (record, checksum)
                else:
                    pending, expected = waiting
                    if (record["committed"] != serial or record["record_sha256"] != expected
                            or type(record["after_data_fsync_monotonic_ns"]) is not int
                            or record["after_data_fsync_monotonic_ns"] <= 0):
                        raise ValueError("unmatched durability marker")
                    committed.append(dict(pending, data_sha256=expected, durability=record))
                    waiting = None
            except (ValueError, TypeError, KeyError) as exc:
                tail = {"reason": str(exc), "bytes_at_first_invalid_frame": len(raw)}
                break
    if waiting is not None:
        tail = dict(tail or {}, unconfirmed=waiting[0], data_sha256=waiting[1])
    return committed, tail


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    args = parser.parse_args()
    records, tail = recover(args.journal)
    print(json.dumps({"committed": records, "unconfirmed_tail": tail}, indent=2))
