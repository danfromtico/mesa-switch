#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Correlate Windows GM20B DMA evidence and checkpoint observed attempts."""
import argparse
import json
from pathlib import Path
import struct

from dma_attempt_journal import STAGES, digest, recover


# Verified 64-bit schemas for the current Windows DxgKrnl manifest. Keep the
# source bytes in each checkpoint so the recovery process can recalculate joins.
SCHEMAS = {
    (27, 2): ("<QQIQ", "process adapter client_type device", 60),
    (29, 2): ("<QQIQ", "process adapter client_type device", 60),
    (30, 0): ("<QI28xQ", "device node context", 64),
    (31, 0): ("<QI28xQ", "device node context", 64),
    (32, 0): ("<QI28xQ", "device node context", 64),
    (175, 1): ("<Q8xIQI", "context packet_type dma queue", 44),
    (176, 0): ("<QIQII", "context packet_type dma queue preempted", 28),
    (177, 1): ("<QIQII12xI", "context packet_type dma queue irq page_fault", 52),
    (178, 1): ("<QIIQ8xI8xQ", "context packet_type queue dma_bytes present queue_packet", 60),
    (180, 1): ("<QIIIIQ", "context packet_type queue preempted timed_out queue_packet", 32),
    (433, 1): ("<Q8xQ", "context kmd_context", 32),
}


def fields(event):
    raw = bytes.fromhex(event["payload_hex"])
    kind = (event["event_id"], event["version"])
    if event["provider"] == "TegraKMD" and kind == (7, 2) and len(raw) == 32:
        return dict(zip("luid kmd_context dma dma_bytes phase detail".split(),
                        struct.unpack("<QQIIII", raw)))
    if event["provider"] != "DxgKrnl":
        raise ValueError("unsupported source provider")
    if kind == (110, 1):
        adapter, config = struct.unpack_from("<QI", raw)
        if len(raw) != 56 + config:
            raise ValueError("unexpected adapter schema length")
        return {"adapter": adapter, "luid": struct.unpack_from("<Q", raw, len(raw)-8)[0]}
    fmt, names, size = SCHEMAS[kind]
    if len(raw) != size:
        raise ValueError("unexpected DxgKrnl schema length")
    return dict(zip(names.split(), struct.unpack_from(fmt, raw)))


def identify(meta, identity_events, submit):
    adapter, device, ctx, association, queue = identity_events
    evidence = (*identity_events, submit)
    codes = tuple(e["event_id"] for e in evidence)
    if (any(e["provider"] != "DxgKrnl" for e in evidence)
        or codes not in ((110,27,30,433,178,175),(110,29,32,433,178,175))):
        raise ValueError("missing identity source")
    a, d, c, private, q, s = map(fields, (*identity_events, submit))
    qpc = [e["source_qpc"] for e in (*identity_events, submit)]
    if (not qpc[0] < qpc[1] < qpc[2] <= qpc[3] < qpc[4] < qpc[5]
            or a["luid"] != meta["adapter_luid"] or a["adapter"] != d["adapter"]
            or d["device"] != c["device"] or c["context"] != private["context"]
            or c["context"] != q["context"] or c["context"] != s["context"]
            or q["queue"] != s["queue"] or q["packet_type"] or s["packet_type"]
            or not all((c["context"], private["kmd_context"], q["queue_packet"], q["dma_bytes"], s["dma"]))
            or d["process"] != queue["pid"]
            or (codes[2] == 30 and ctx["pid"] != d["process"])
            or (codes[2] == 32 and not (device["pid"],device["tid"]) ==
                                          (ctx["pid"],ctx["tid"]) ==
                                          (association["pid"],association["tid"]))):
        raise ValueError("scheduler identity does not match same-boot context and queue")
    return {"boot":meta["boot"], "capture":meta["capture"], "luid":a["luid"], "node":c["node"],
            "context":c["context"], "context_start_qpc":qpc[2], "kmd_context":private["kmd_context"],
            "queue":q["queue"], "queue_packet":q["queue_packet"], "dma":s["dma"],
            "dma_bytes":q["dma_bytes"], "owner_pid":d["process"]}


def validate(snapshot):
    stages = snapshot["stages"]
    identity = snapshot["identity"]
    expected = identify(snapshot["capture"],snapshot["identity_events"],stages["submit"])
    if expected != identity or any(name not in STAGES for name in stages):
        raise ValueError("invalid attempt identity")
    submit = stages["submit"]
    entry = stages.get("kmd_entry")
    if ("kmd_return" in stages or "gpu_complete" in stages) and not entry:
        raise ValueError("KMD entry missing")
    if "retire" in stages and not {"driver_notification", "dma_stop"} <= stages.keys():
        raise ValueError("Windows retirement chain missing")
    for name,event in stages.items():
        values = fields(event)
        if name == "submit":
            continue
        if name in ("kmd_entry", "kmd_return", "gpu_complete"):
            phase = {"kmd_entry":1,"kmd_return":5,"gpu_complete":4}[name]
            if (event["provider"] != "TegraKMD" or values["phase"] != phase
                or any(values[k] != identity[k] for k in ("luid","kmd_context","dma","dma_bytes"))
                or not submit["source_qpc"] < entry["source_qpc"] <= event["source_qpc"]
                or (name != "gpu_complete" and (event["pid"],event["tid"]) != (submit["pid"],submit["tid"]))):
                raise ValueError("foreign or out-of-order KMD event")
            continue
        wanted = {"driver_notification":177,"dma_stop":176,"retire":180}[name]
        if ((event["provider"],event["event_id"]) != ("DxgKrnl",wanted)
                or values["context"] != identity["context"] or values["queue"] != identity["queue"]
                or values["packet_type"] or event["source_qpc"] <= submit["source_qpc"]
                or (name != "retire" and values["dma"] != identity["dma"])
                or (name == "driver_notification" and (values["irq"] != 1 or values["page_fault"]))
                or (name == "retire" and (values["queue_packet"] != identity["queue_packet"]
                                           or values["preempted"] or values["timed_out"]))):
            raise ValueError("foreign Windows completion event")
    if "retire" in stages:
        stop = stages["dma_stop"]
        irq = stages["driver_notification"]
        retired = stages["retire"]
        if (fields(stop)["preempted"] or
                not irq["source_qpc"] <= stop["source_qpc"] <= retired["source_qpc"] or
                ("gpu_complete" in stages and stages["gpu_complete"]["source_qpc"] > irq["source_qpc"])):
            raise ValueError("retirement is ambiguous or preempted")
    return (set(stages) == set(STAGES)
            and fields(stages["kmd_return"])["detail"] == 0)


class Capture:
    def __init__(self, journal, metadata, *, fixture_filter=False):
        self.journal = journal
        self.meta = metadata
        self.fixture_filter = fixture_filter
        self.adapters = {}
        self.devices = {}
        self.contexts = {}
        self.private = {}
        self.queued = {}
        self.attempts = {}
        self.by_fence = {}
        self.seen_fences = set()
        self.latest = {}
        self.qpc = 0

    def add_stage(self, key, stage, event):
        old = self.attempts[key]
        if stage in old["stages"]:
            raise ValueError("duplicate source stage")
        snapshot = dict(old, revision=old["revision"]+1, stages=dict(old["stages"], **{stage:event}))
        snapshot["complete"] = validate(snapshot)
        self.journal.commit(snapshot)
        self.attempts[key] = snapshot

    def observe(self, event):
        qpc = event["source_qpc"]
        if qpc < self.qpc:
            raise ValueError("source events reordered; a complete capture cannot be certified")
        self.qpc = qpc
        kind = event["event_id"]
        row = fields(event)
        if event["provider"] == "TegraKMD":
            key = self.by_fence.get((row["luid"],row["kmd_context"],row["dma"]))
            stage = {1:"kmd_entry", 4:"gpu_complete", 5:"kmd_return"}.get(row["phase"])
            if key and stage:
                self.add_stage(key,stage,event)
            return
        if kind == 110 and row["luid"] == self.meta["adapter_luid"]:
            self.adapters[row["adapter"]] = event
        if kind in (27,29) and row["adapter"] in self.adapters:
            self.devices[row["device"]] = event
        if kind in (30,32) and row["device"] in self.devices:
            ctx = row["context"]
            previous = self.contexts.get(ctx)
            if previous is not None and fields(previous) != row:
                raise ValueError("ambiguous context identity")
            if previous is None:
                self.contexts[ctx] = event
        if kind == 31:
            self.contexts.pop(row["context"],None)
            association = self.private.pop(row["context"],None)
            if association:
                private = fields(association)["kmd_context"]
                self.by_fence = {key:value for key,value in self.by_fence.items() if key[1] != private}
            for key in list(self.latest):
                if key[0] == row["context"]:
                    self.latest.pop(key)
                    self.queued.pop(key,None)
            return
        if kind == 433 and row["context"] in self.contexts and row["kmd_context"]:
            previous = self.private.get(row["context"])
            if previous is not None and fields(previous) != row:
                raise ValueError("context has ambiguous private mapping")
            if previous is None:
                self.private[row["context"]] = event
        if kind not in (175,176,177,178,180) or row["packet_type"]:
            return
        ctx = row["context"]
        if ctx not in self.contexts:
            return
        qkey = (ctx,row["queue"])
        selected = (not self.fixture_filter or event["pid"] == self.meta.get("dwm")
                    or (row.get("present") and row.get("dma_bytes") == 52))
        if kind == 178 and selected:
            if qkey in self.queued:
                raise ValueError("ambiguous queue enqueue")
            self.queued[qkey] = event
        if qkey not in self.queued or ctx not in self.private:
            return
        if kind == 175:
            device = self.devices[fields(self.contexts[ctx])["device"]]
            adapter = self.adapters[fields(device)["adapter"]]
            evidence = [adapter,device,self.contexts[ctx],self.private[ctx],self.queued[qkey]]
            identity = identify(self.meta,evidence,event)
            key = digest(identity)
            fence = (identity["luid"],identity["kmd_context"],identity["dma"])
            if key in self.attempts or fence in self.seen_fences:
                raise ValueError("duplicate scheduler identity")
            previous = self.latest.get(qkey)
            previous_stop = self.attempts[previous]["stages"].get("dma_stop") if previous else None
            if previous and (previous_stop is None or not fields(previous_stop)["preempted"]):
                raise ValueError("queue retry without a matching preempted stop")
            snapshot = {"kind":"attempt","identity":identity,"identity_events":evidence,"capture":self.meta,
                        "revision":1,"complete":False,"stages":{"submit":event}}
            validate(snapshot)
            self.journal.commit(snapshot)
            self.attempts[key] = snapshot
            self.by_fence[fence] = key
            self.seen_fences.add(fence)
            self.latest[qkey] = key
        elif kind in (176,177):
            key = self.latest.get(qkey)
            if key and self.attempts[key]["identity"]["dma"] == row["dma"]:
                if kind == 176 or (row["irq"] == 1 and not row["page_fault"]):
                    self.add_stage(key,"dma_stop" if kind==176 else "driver_notification",event)
        elif kind == 180 and not row["preempted"] and not row["timed_out"]:
            key = self.latest.get(qkey)
            if key and {"dma_stop","driver_notification"} <= self.attempts[key]["stages"].keys():
                self.add_stage(key,"retire",event)
            self.queued.pop(qkey,None)
            self.latest.pop(qkey,None)


def read_attempts(path):
    checkpoints, tail = recover(path)
    by_attempt = {}
    active_queue_attempts = {}
    capture_end = None
    ready = None
    for checkpoint in checkpoints:
        record = checkpoint["snapshot"]
        if record["kind"] == "capture_ready" and ready is None and not by_attempt:
            ready = record["capture"]
            continue
        if record["kind"] == "capture_end" and capture_end is None and record["capture"] == ready:
            capture_end = record
            continue
        if (record["kind"] != "attempt" or record["capture"] != ready or capture_end is not None
                or validate(record) != record["complete"]):
            raise ValueError("invalid attempt checkpoint")
        key = digest(record["identity"])
        earlier = by_attempt.get(key)
        qkey = tuple(record["identity"][field] for field in
                     ("context","context_start_qpc","queue","queue_packet"))
        if earlier:
            prior = earlier["snapshot"]
            if (record["capture"] != prior["capture"] or record["identity_events"] != prior["identity_events"]
                    or record["revision"] != prior["revision"]+1
                    or len(record["stages"]) != len(prior["stages"])+1
                    or any(record["stages"].get(k) != v for k,v in prior["stages"].items())):
                raise ValueError("checkpoint is not an extension of the same attempt")
        else:
            if record["revision"] != 1 or set(record["stages"]) != {"submit"}:
                raise ValueError("attempt initiation checkpoint missing")
            previous_key = active_queue_attempts.get(qkey)
            if previous_key:
                stop = by_attempt[previous_key]["snapshot"]["stages"].get("dma_stop")
                if stop is None or not fields(stop)["preempted"]:
                    raise ValueError("attempt reuses an active queue without preemption")
            active_queue_attempts[qkey] = key
        if "retire" in record["stages"] and active_queue_attempts.get(qkey) != key:
            raise ValueError("retirement belongs to a different queue attempt")
        by_attempt[key] = checkpoint
    return {"attempts":list(by_attempt.values()), "capture_ready":ready,
            "capture_end":capture_end,"unconfirmed_tail":tail,
            "committed_checkpoints":len(checkpoints)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", type=Path)
    args = parser.parse_args()
    print(json.dumps(read_attempts(args.journal),indent=2))
