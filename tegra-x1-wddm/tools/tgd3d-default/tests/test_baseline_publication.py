#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise selection and a plain Git push against an isolated local bare repository."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=2) + "\n").encode())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exercise(shell, work):
    source = Path(__file__).resolve().parents[4]
    repo, remote = work / "checkout", work / "remote.git"
    subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
    subprocess.run(["git", "init", "-q", "-b", "tgd3d-vulkan", str(repo)], check=True)

    def run(*args, succeeds=True, contains=None):
        p = subprocess.run(args, cwd=repo, capture_output=True, text=True, errors="replace")
        output = p.stdout + p.stderr
        if (p.returncode == 0) != succeeds or (contains and contains not in output):
            raise RuntimeError(f"{args[0]} returned {p.returncode}\n{output[-2400:]}")
        return p.stdout.strip()

    def commit(message):
        run("git", "add", "--all")
        run("git", "commit", "-q", "-m", message)
        return run("git", "rev-parse", "HEAD")

    def remote_head():
        return run("git", "ls-remote", "origin", "refs/heads/tgd3d-vulkan").split()[0]

    tools = Path("tegra-x1-wddm/tools")
    gate = str(tools / "Select-GraphicsBaseline.ps1")
    hook = str(tools / "check_graphics_baseline_push.py")
    run("git", "config", "user.name", "Switch policy test")
    run("git", "config", "user.email", "test@example.invalid")
    run("git", "config", "core.hooksPath", str(repo / ".git/hooks"))
    run("git", "config", "switch.graphicsGatePwsh", shell)
    run("git", "remote", "add", "origin", str(remote))
    (repo / "shader.cpp").write_text("// accepted\n")
    base_source = commit("Initial source")
    for name in (gate, hook, str(tools / "tgd3d-default/Baseline-Promotion.ps1")):
        (repo / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, repo / name)
    docs = Path("tegra-x1-wddm/docs")
    accepted_path = repo / docs / "graphics-baseline.json"
    ledger_path = repo / docs / "open-faults.json"
    accepted = json.loads((source / docs / "graphics-baseline.json").read_bytes())
    accepted["source_commit"] = base_source
    accepted["qualification"] = (docs / "initial-qualification.json").as_posix()
    write(repo / docs / "case.json", {"result": "test case passed"})
    write(repo / docs / "boot.json", {"intent": 105, "boot": 110})
    shutil.copyfile(source / docs / ".gitattributes", repo / docs / ".gitattributes")
    case_hash = sha(repo / docs / "case.json")
    initial_report = {"source_commit": base_source, "directory": accepted["directory"], "components": accepted["components"],
                      "cases": [{"evidence_file": "case.json", "evidence_sha256": case_hash}],
                      "system_log": {"restarts": [{"evidence_file": "boot.json", "evidence_sha256": sha(repo / docs / "boot.json")}]}}
    write(repo / accepted["qualification"], initial_report)
    accepted["qualification_sha256"] = sha(repo / accepted["qualification"])
    write(accepted_path, accepted)
    fault = {"id": "TEST-PRESENT", "status": "open", "kind": "restart", "affected_scopes": ["presentation"],
             "trigger": "Present", "evidence": [(docs / "case.json").as_posix()], "cause": "unknown",
             "daily_workaround": "Use qualified path", "close_when": "Verify original trigger", "resolution": None}
    ledger = {"schema": 1, "faults": [fault], "independent_qualifications": []}
    write(ledger_path, ledger)
    original = commit("Previously accepted baseline")
    run("git", "push", "-q", "origin", "HEAD:refs/heads/tgd3d-vulkan")
    run(sys.executable, hook, "--install")
    (repo / "shader.cpp").write_text("// candidate\n")
    src = commit("Independent shader change")
    names = run("git", "diff", "--name-only", base_source, src).splitlines()
    candidate = dict(accepted["components"], **{"tg_d3d_arm64.dll": "a" * 64})
    file = (docs / "qualification.json").as_posix()
    claim = {"schema": 1, "source_commit": src, "base_commit": base_source, "changed_paths": names,
             "directory": r"C:\ProgramData\SwitchWOA\GpuUpdate\test", "components": candidate, "change_kind": "presentation",
             "trial": {"one_time": True, "power": "Online", "gpu_code": 0, "boot_record": 110, "loaded": candidate},
             "system_log": {"complete": True, "first_record": 100, "last_record": 125, "boot_records": [110],
                            "abnormal_events": [], "restarts": [{"explained": True, "kind": "controlled", "cause": "One-time trial",
                                "boot_record": 110, "system_records": [105, 110], "evidence_file": "boot.json",
                                "evidence_sha256": sha(repo / docs / "boot.json")}]},
             "cases": [{"name": "shader run", "passed": True, "before_boot": 110, "after_boot": 110,
                        "output_verified": True, "normal_exit": True, "desktop_available": True,
                        "evidence_file": "case.json", "evidence_sha256": case_hash}]}
    write(repo / file, claim)
    run(shell, "-NoProfile", "-File", gate, "-Action", "Select", "-Qualification", file,
        succeeds=False, contains="Open faults block")
    assert json.loads(accepted_path.read_bytes()) == accepted
    unqualified = {"schema": 1, "source_commit": src, "directory": claim["directory"], "components": candidate,
                   "qualification": file, "qualification_sha256": sha(repo / file)}
    write(accepted_path, unqualified)
    (repo / gate).write_text("# Deliberately inert candidate verifier used only by this negative test.\n")
    commit("Attempt to publish without the qualification gate")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="Open faults block")
    assert remote_head() == original
    write(accepted_path, accepted)
    commit("Return the tip to the prior selection")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="Open faults block")
    assert remote_head() == original
    # Drop the rejected private history in this isolated test repository.
    run("git", "reset", "--hard", src)
    claim["change_kind"] = "independent"
    write(repo / file, claim)
    ledger["independent_qualifications"] = [{"evidence_sha256": sha(repo / file), "independence": [
        {"fault_id": fault["id"], "reason": "Isolated shader control", "test_sha256": case_hash}]}]
    write(ledger_path, ledger)
    commit("Bind independent evidence to the fault register")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Select", "-Qualification", file)
    selected = commit("Select the independently qualified package")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Publish")
    assert remote_head() == selected
    ledger["faults"].append(dict(fault, id="TEST-NEW"))
    write(ledger_path, ledger)
    updated = commit("Register another fault")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan")
    assert remote_head() == updated
    run("git", "checkout", "-q", "-b", "older-source", original)
    (repo / "older-source.cpp").write_text("// Qualified source based on the former baseline\n")
    commit("Source on the former baseline")
    run("git", "checkout", "-q", "tgd3d-vulkan")
    run("git", "merge", "--no-ff", "-q", "-m", "Merge source into the current baseline", "older-source")
    updated = run("git", "rev-parse", "HEAD")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan")
    assert remote_head() == updated
    # The production builder is a linked worktree of a shallow, filtered Mesa clone.
    # Its proof files are present while unrelated shader history can remain unfetched.
    partial = work / "partial"
    run("git", "--git-dir", str(remote), "config", "uploadpack.allowFilter", "true")
    run("git", "clone", "-q", "--no-checkout", "--depth=3", "--filter=blob:none", "-b", "tgd3d-vulkan",
        remote.as_uri(), str(partial))
    run("git", "-C", str(partial), "sparse-checkout", "set", "--no-cone",
        "/tegra-x1-wddm/docs/", "/tegra-x1-wddm/tools/")
    run("git", "-C", str(partial), "checkout", "-q", "tgd3d-vulkan")
    assert run("git", "-C", str(partial), "rev-parse", "--is-shallow-repository") == "true"
    missing = run("git", "-C", str(partial), "rev-list", "--objects", "--missing=print", "HEAD").splitlines()
    assert any(line.startswith("?") for line in missing)
    run("git", "-C", str(partial), "config", "switch.graphicsGatePwsh", shell)
    lines = f"HEAD {updated} refs/heads/tgd3d-vulkan {selected}\n"
    subprocess.run([sys.executable, str(partial / hook)], cwd=partial, input=lines, text=True,
                   capture_output=True, check=True)
    write(repo / docs / "boot.json", {"intent": 108, "boot": 110})
    commit("Accidental replacement of a retained hardware receipt")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="mismatched hardware evidence")
    assert remote_head() == updated
    run("git", "reset", "--hard", updated)
    run("git", "push", "--force", "origin", f"{original}:refs/heads/tgd3d-vulkan",
        succeeds=False, contains="Cannot rewrite")
    assert remote_head() == updated
    ledger["faults"].pop(0)
    ledger["independent_qualifications"] = []
    write(ledger_path, ledger)
    commit("Try to erase an unresolved fault")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="disappeared")
    assert remote_head() == updated
    print("PASS blocked selection and push, qualified independent publication, new fault registration and deletion guard")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pwsh", default=shutil.which("pwsh"))
    args = parser.parse_args()
    if not args.pwsh:
        parser.error("Specify the PowerShell executable with --pwsh")
    with tempfile.TemporaryDirectory(prefix="graphics-promotion-test-") as directory:
        exercise(args.pwsh, Path(directory))
