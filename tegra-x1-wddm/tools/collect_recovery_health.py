#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect small, read-only Windows recovery records over an existing SSH login.

Use native tools instead of the PowerShell/CIM inventory that exhausted the
Lite's commit limit on 2026-11-13. Event queries have explicit record limits;
raw output streams to local files. A client timeout does not prove that the
remote command exited: record that uncertainty and stop, without a retry.
"""

import argparse
import datetime
import json
from pathlib import Path
import subprocess


QUERIES = (
    ("memory", "wmic.exe OS get FreePhysicalMemory,FreeVirtualMemory,"
     "TotalVirtualMemorySize,TotalVisibleMemorySize /value"),
    ("processes", "tasklist.exe /fo csv /nh"),
    ("fan-cpu", r"reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\switchfan\Parameters"),
    ("windows-cpu", r"reg.exe query HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0"),
    ("sd-device", 'pnputil.exe /enum-devices /instanceid "ACPI\\NVDA0101\\0" /drivers'),
    ("sd-service", r"reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc /s"),
    ("sd-sys-hash", r"certutil.exe -hashfile C:\Windows\System32\drivers\nvtsdhc.sys SHA256"),
    ("crash-settings", r"reg.exe query HKLM\SYSTEM\CurrentControlSet\Control\CrashControl"),
    ("volmgr-events", 'wevtutil.exe qe System /q:"*[System['
     'Provider[@Name=\'volmgr\']]]" /rd:true /c:12 /f:xml'),
    ("resource-events", 'wevtutil.exe qe System /q:"*[System[(EventID=2004 or '
     'EventID=41 or EventID=6008 or EventID=1074 or EventID=12)]]" '
     '/rd:true /c:24 /f:xml'),
    ("application-events", 'wevtutil.exe qe Application /q:"*[System['
     '(EventID=1000 or EventID=1001 or EventID=1002)]]" /rd:true /c:12 /f:xml'),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Existing SSH host or user@host")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--host-key-alias")
    parser.add_argument("--out", type=Path, required=True, help="New local output directory")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
           "-o", "ConnectTimeout=15", "-o", "ServerAliveInterval=5",
           "-o", "ServerAliveCountMax=2"]
    if args.identity:
        ssh += ["-i", str(args.identity)]
    if args.host_key_alias:
        ssh += ["-o", "HostKeyAlias=" + args.host_key_alias]
    ssh += ["--", args.host]
    for name, command in QUERIES:
        record = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "command": command, "remote_completion": "unknown"}
        with (args.out / (name + ".stdout")).open("xb") as stdout, \
                (args.out / (name + ".stderr")).open("xb") as stderr:
            try:
                result = subprocess.run(ssh + [command], stdout=stdout,
                                        stderr=stderr, timeout=25)
                record["exit_code"] = result.returncode
                if result.returncode != 255:
                    record["remote_completion"] = "exit_status_received"
            except subprocess.TimeoutExpired:
                record["local_timeout_seconds"] = 25
            except KeyboardInterrupt:
                record["local_interrupted"] = True
            except OSError as error:
                record["launch_error"] = str(error)
        record["stdout_bytes"] = (args.out / (name + ".stdout")).stat().st_size
        record["stderr_bytes"] = (args.out / (name + ".stderr")).stat().st_size
        (args.out / (name + ".json")).write_text(
            json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(name + ": " + record["remote_completion"] +
              ", exit=" + str(record.get("exit_code", "unknown")), flush=True)
        if record.get("exit_code") != 0:
            print("Stopped. Read the saved result before any further remote action.")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
