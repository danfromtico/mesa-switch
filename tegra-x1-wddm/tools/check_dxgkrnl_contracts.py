#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the WSL comment inventory, frozen hardware citations and single-question Switch rounds."""
import argparse
from collections import Counter
from datetime import datetime
from hashlib import sha1, sha256
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "tegra-x1-wddm/docs"
STATUSES = {"已满足", "已违反", "未知"}
SCOPES = {"原生可比", "WSL专属", "源码元信息"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verified_json(path, expected):
    data = path.read_bytes()
    require(sha256(data).hexdigest() == expected, f"File digest differs for {path}")
    return json.loads(data)


def doc_path(relative):
    path = (DOCS / relative).resolve()
    require(path.is_relative_to(DOCS) and not Path(relative).is_absolute(), f"Expected a documentation path: {relative}")
    return path


def resolve_pointer(document, pointer):
    require(pointer.startswith("/") and len(pointer) > 1, f"Expected a specific evidence field: {pointer}")
    for key in pointer[1:].split("/"):
        key = key.replace("~1", "/").replace("~0", "~")
        if isinstance(document, list):
            require(key.isdecimal(), f"Invalid array index in {pointer}")
            document = document[int(key)]
        else:
            document = document[key]
    return document


def check_sources(catalog, source_directory):
    expected_counts = {f["file"]: f["comments"] for f in catalog["files"]}
    require(len(expected_counts) == len(catalog["files"]), "Duplicate source file")
    tree_entries = []
    for record in sorted(catalog["files"], key=lambda f: f["file"].encode()):
        name = record["file"]
        require(Path(name).name == name and re.fullmatch("[0-9a-f]{40}", record["git_blob"]), "Invalid Git leaf " + name)
        tree_entries.append(b"100644 " + name.encode() + b"\0" + bytes.fromhex(record["git_blob"]))
    tree = b"".join(tree_entries)
    require(sha1(b"tree " + str(len(tree)).encode() + b"\0" + tree).hexdigest() == catalog["tree"], "Git source tree differs")
    rows = catalog["rows"]
    require(Counter(r["file"] for r in rows) == Counter(expected_counts), "Comment inventory coverage differs")
    require(len(rows) == catalog["comment_count"], "Total comment count differs")
    files = {}
    if source_directory:
        for record in catalog["files"]:
            content = (source_directory / record["file"]).read_bytes()
            require(sha256(content).hexdigest() == record["sha256"], record["file"] + " differs from pinned source")
            require(sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest() == record["git_blob"],
                    record["file"] + " differs from pinned Git blob")
            files[record["file"]] = content.splitlines(keepends=True)
    ids = {}
    inventory = []
    for row in rows:
        start, column = row["start_line"], row["start_column"]
        end, end_column = row["end_line"], row["end_column"]
        identity = f"{row['file']}:{start}:{column}"
        require(row["id"] == identity and identity not in ids, "Duplicate or invalid location " + identity)
        require(min(start, column, end, end_column) > 0 and (start, column) <= (end, end_column), identity + " invalid span")
        require(bool(re.fullmatch("[0-9a-f]{64}", row["comment_sha256"])), identity + " invalid hash")
        require(row["status"] in STATUSES and row["scope"] in SCOPES, identity + " invalid status or scope")
        require(all(isinstance(row[k], str) and row[k].strip() for k in ("assertion", "reason", "observable")),
                identity + " missing assertion or assessment")
        if files:
            lines = files[row["file"]]
            data = b"".join(lines[start - 1:end - 1]) + lines[end - 1][:end_column]
            require(sha256(data[column - 1:]).hexdigest() == row["comment_sha256"], identity + " source bytes differ")
        inventory.append(f"{identity}\t{end}\t{end_column}\t{row['comment_sha256']}\n")
        ids[identity] = row
    require(sha256("".join(inventory).encode()).hexdigest() == catalog["locations_sha256"], "Inventory digest differs")
    reviewer = catalog["reviewer"]
    require(reviewer["model"] == "gemini-4-neon-xhigh" and reviewer["effort"] == "xhigh", "Reviewer preset differs")
    offset = 0
    for packet in reviewer["packets"]:
        count = packet["count"]
        require(count > 0 and offset + count <= len(rows), "Invalid reviewer packet length")
        require(packet["first"] == rows[offset]["id"] and packet["last"] == rows[offset + count - 1]["id"],
                "Reviewer packet range differs")
        require(all(re.fullmatch("[0-9a-f]{64}", packet[k]) for k in ("request_sha256", "native_response_sha256")),
                "Invalid reviewer handback digest")
        offset += count
    require(offset == len(rows), "Missing reviewer packet")
    return ids


def check_rounds(section, ids, faults):
    rounds = {}
    for row in section["rounds"]:
        key, identity = row["round_id"], row["contract_id"]
        require(key not in rounds and isinstance(identity, str) and identity in ids and "contract_ids" not in row,
                f"{key} needs one declared source location")
        plan = verified_json(doc_path(row["declaration"]), row["declaration_sha256"])
        require(plan["round_id"] == key and plan["contract_id"] == identity and "contract_ids" not in plan, key + " plan differs")
        require(plan["source_commit"] == section["source_commit"] and plan["comment_sha256"] == ids[identity]["comment_sha256"],
                key + " unbound source")
        require(plan["related_fault"] in faults and all(plan[k] for k in ("actor", "predicate", "required_observation",
                "satisfied_if", "violated_if", "unknown_if")), key + " incomplete plan")
        declared = datetime.fromisoformat(plan["declared_utc"])
        require(declared.utcoffset() is not None, key + " declaration needs a time zone")
        require(row["state"] in ("declared", "recorded") and row["status"] in STATUSES, key + " invalid ledger state")
        if row["state"] == "declared":
            require(row["status"] == "未知" and "receipt" not in row, key + " unmeasured verdict")
        else:
            report = verified_json(doc_path(row["receipt"]), row["receipt_sha256"])
            require(report["round_id"] == key and report["contract_id"] == identity and "contract_ids" not in report,
                    key + " receipt adjudicates a different question")
            require(report["status"] == row["status"] and report["declaration_sha256"] == row["declaration_sha256"],
                    key + " receipt differs from predeclared plan")
            started = datetime.fromisoformat(report["started_utc"])
            require(started.utcoffset() is not None, key + " measurement needs a time zone")
            if started < declared:
                require(re.fullmatch("[0-9a-f]{64}", report.get("board_issuance_sha256", "")) and report.get("chronology"),
                        key + " clock discrepancy requires the predeclared board issuance record")
            if row["status"] != "未知":
                require(report["target_hit"] is True and report["unexplained_restart"] is False and report["observation"],
                        key + " conclusive verdict requires a measured target")
                before, after = report["before"], report["after"]
                require(before["boot"] and before["dwm"] and all(before[k] == after[k] for k in ("boot", "dwm", "dwm_start"))
                        and all(v["ac"] == "Online" and v["code"] == 0 for v in (before, after)),
                        key + " hardware or desktop continuity evidence differs")
        rounds[key] = row
    for item in section.get("unobserved_recovery_boots", []):
        report = verified_json(doc_path(item["receipt"]), item["receipt_sha256"])
        key = item["round_id"]
        require(key in rounds and report["round_id"] == key and report["contract_id"] == rounds[key]["contract_id"],
                key + " recovery sequence belongs to a different case")
        intervals = report["unobserved_intervals"]
        require(item["intervals"] == [[r["from_boot"], r["to_boot"]] for r in intervals],
                key + " recovery intervals differ")
        require(all(r["from_boot"] < r["to_boot"] and not r["game_reissued_by_test"] and r["cause"] == item["cause"]
                    for r in intervals), key + " intervening boots must retain independent attribution")
    return rounds


def check_evidence(catalog, ids, rounds):
    frozen = {}
    for key, item in catalog["evidence_index"].items():
        commit, path = item["commit"], item["path"]
        require(re.fullmatch("[0-9a-f]{40}", commit) and path.startswith("tegra-x1-wddm/docs/"), "Invalid citation " + key)
        raw = subprocess.check_output(["git", "-C", str(ROOT), "show", commit + ":" + path])
        require(sha256(raw).hexdigest() == item["sha256"], key + " archived evidence differs")
        frozen[key] = json.loads(raw)
    for identity, row in ids.items():
        cited = row["evidence"]
        measured = row.get("hardware_rounds", [])
        require(isinstance(cited, list) and isinstance(measured, list), identity + " invalid evidence")
        for citation in cited:
            key, _, pointer = citation.partition("#")
            require(key in frozen, identity + " unknown evidence " + key)
            if pointer:
                resolve_pointer(frozen[key], pointer)
            else:
                require(row["status"] == "未知", identity + " conclusive finding needs a specific evidence field")
        for key in measured:
            require(key in rounds and rounds[key]["contract_id"] == identity and rounds[key]["state"] == "recorded",
                    identity + " unbound physical round")
        if row["status"] != "未知":
            require(row["scope"] == "原生可比" and (cited or any(rounds[k]["status"] == row["status"] for k in measured)),
                    identity + " lacks evidence for a native verdict")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-directory", type=Path, help="pinned drivers/hv/dxgkrnl checkout for per-comment byte checks")
    args = parser.parse_args()
    ledger = json.loads((DOCS / "open-faults.json").read_bytes())
    section = ledger["dxgkrnl_contracts"]
    catalog = json.loads(doc_path(section["catalog"]).read_bytes())
    require(catalog["commit"] == section["source_commit"] and catalog["tree"] == section["source_tree"], "WSL revision mismatch")
    ids = check_sources(catalog, args.source_directory)
    rounds = check_rounds(section, ids, {f["id"] for f in ledger["faults"]})
    check_evidence(catalog, ids, rounds)
    counts = Counter(row["status"] for row in ids.values())
    print(f"{len(ids)} comment records, {len(rounds)} single-question hardware rounds, "
          f"{counts['已满足']} satisfied, {counts['已违反']} violated, {counts['未知']} unknown")


if __name__ == "__main__":
    main()
