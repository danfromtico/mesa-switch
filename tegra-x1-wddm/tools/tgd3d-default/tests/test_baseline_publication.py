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
    run("git", "checkout", "-q", "-b", "older-source", original)
    (repo / "older-source.cpp").write_text("// Qualified source based on the former baseline\n")
    commit("Source on the former baseline")
    run("git", "checkout", "-q", "tgd3d-vulkan")
    run("git", "merge", "--no-ff", "-q", "-m", "Merge source into the current baseline", "older-source")
    updated = run("git", "rev-parse", "HEAD")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan")
    assert remote_head() == updated
    run("git", "checkout", "-q", "-b", "old-baseline-view", original)
    (repo / "retained-source.cpp").write_text("// Source on the old selection\n")
    commit("Keep an old selection on another branch")
    run("git", "merge", "-s", "ours", "--no-ff", "-q", "-m", "Keep the old selection", "tgd3d-vulkan")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="must advance")
    run("git", "checkout", "tgd3d-vulkan", "--", str(docs))
    commit("Restore the destination selection after the intermediate rollback")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="must advance")
    run("git", "checkout", "-q", "tgd3d-vulkan")
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

    run("git", "reset", "--hard", updated)
    ledger = json.loads(ledger_path.read_bytes())
    previous = json.loads(accepted_path.read_bytes())
    implementation = repo / "src/nouveau/tgd3d/tgd3d_dxgi.cpp"
    implementation.parent.mkdir(parents=True, exist_ok=True)
    implementation.write_text("// Repair the observed runtime input guard\n")
    repair_source = commit("Scoped input repair")
    repaired = dict(previous["components"], **{"tg_d3d_arm64.dll": "6" * 64})
    supporting = []
    for kind in ("cause", "review", "build"):
        path = repo / docs / f"repair-{kind}.json"
        write(path, {"kind": kind, "source_commit": repair_source, "components": repaired})
        supporting.append({"evidence_file": path.name, "evidence_sha256": sha(path)})
    write(repo / docs / "repair-case.json", {"input": 1, "callback_return": 0, "boot": 210})
    write(repo / docs / "repair-boot.json", {"intent": 205, "boot": 210})
    scoped = json.loads(json.dumps(claim))
    scoped.update(change_kind="verified-fix", source_commit=repair_source,
                  base_commit=previous["source_commit"], components=repaired,
                  changed_paths=run("git", "diff", "--name-only", previous["source_commit"], repair_source).splitlines())
    scoped["trial"].update(boot_record=210, loaded=repaired)
    scoped["system_log"].update(first_record=200, last_record=220, boot_records=[210])
    scoped["system_log"]["restarts"][0].update(boot_record=210, system_records=[205, 210],
        evidence_file="repair-boot.json", evidence_sha256=sha(repo / docs / "repair-boot.json"))
    scoped["cases"][0].update(before_boot=210, after_boot=210, scenario="observed input", original_trigger=True,
        loaded_components=repaired, output_verified=False, evidence_file="repair-case.json", evidence_sha256=sha(repo / docs / "repair-case.json"))
    binding = {"fault_id": "TEST-INPUT", "scope": "observed input", "case_sha256": scoped["cases"][0]["evidence_sha256"],
               "output_required": False,
               "causal_evidence_sha256": supporting[0]["evidence_sha256"],
               "code_review_sha256": supporting[1]["evidence_sha256"], "build_sha256": supporting[2]["evidence_sha256"],
               "evidence": supporting}
    scoped["verified_fix"] = binding
    repair_fault = dict(fault, id="TEST-INPUT", kind="device-removal",
                        evidence=[(docs / "repair-cause.json").as_posix()])
    repair_fault["verified_fix"] = {"scope": binding["scope"], "root_cause": "Runtime input rejected by the guard",
        "root_cause_confirmed": True, "independent_review_passed": True, "build_signed": True,
        "commit": repair_source, "components": repaired, "component": "tg_d3d_arm64.dll", "output_required": False,
        "causal_evidence_file": (docs / "repair-cause.json").as_posix(),
        "code_review_file": (docs / "repair-review.json").as_posix(),
        "build_file": (docs / "repair-build.json").as_posix(),
        **{field: binding[field] for field in ("causal_evidence_sha256", "code_review_sha256", "build_sha256")}}
    ledger["faults"].append(repair_fault)
    scoped_file = (docs / "scoped-qualification.json").as_posix()
    write(repo / scoped_file, scoped)
    ledger["independent_qualifications"].append({"evidence_sha256": sha(repo / scoped_file), "independence": [
        {"fault_id": item["id"], "reason": "Limited input repair with retained fault scope",
         "test_sha256": binding["case_sha256"]} for item in ledger["faults"]]})
    write(ledger_path, ledger)
    commit("Register scoped cause, review, build and native receipt")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Select", "-Qualification", scoped_file)
    scoped_selected = commit("Select the scoped input repair")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Publish")
    assert remote_head() == scoped_selected
    assert json.loads(ledger_path.read_bytes())["faults"][-1]["status"] == "open"
    write(repo / docs / "repair-review.json", {"changed": True})
    commit("Replace a retained independent review")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="mismatched scoped repair evidence")
    assert remote_head() == scoped_selected
    print("PASS scoped repair selection, publication and retained review integrity")
    run("git", "reset", "--hard", scoped_selected)
    (repo / "shader.cpp").write_text("// Later independent shader revision\n")
    later_source = commit("Later independent source")
    later_components = dict(repaired, **{"tg_d3d_arm64.dll": "7" * 64})
    later = json.loads(json.dumps(scoped))
    later.update(change_kind="independent", source_commit=later_source, base_commit=repair_source,
                 components=later_components,
                 changed_paths=run("git", "diff", "--name-only", repair_source, later_source).splitlines())
    del later["verified_fix"]
    write(repo / docs / "later-case.json", {"boot": 310, "result": "working path"})
    write(repo / docs / "later-boot.json", {"intent": 305, "boot": 310})
    later["trial"].update(boot_record=310, loaded=later_components)
    later["system_log"].update(first_record=300, last_record=320, boot_records=[310])
    later["system_log"]["restarts"][0].update(boot_record=310, system_records=[305, 310],
        evidence_file="later-boot.json", evidence_sha256=sha(repo / docs / "later-boot.json"))
    later["cases"][0].update(before_boot=310, after_boot=310, loaded_components=later_components,
        evidence_file="later-case.json", evidence_sha256=sha(repo / docs / "later-case.json"))
    later_file = (docs / "later-qualification.json").as_posix()
    write(repo / later_file, later)
    ledger["independent_qualifications"].append({"evidence_sha256": sha(repo / later_file), "independence": [
        {"fault_id": item["id"], "reason": "Independent later test retains the earlier repair record",
         "test_sha256": later["cases"][0]["evidence_sha256"]} for item in ledger["faults"]]})
    write(ledger_path, ledger)
    commit("Register the later independent qualification")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Select", "-Qualification", later_file)
    later_selected = commit("Select the later package")
    run(shell, "-NoProfile", "-File", gate, "-Action", "Publish")
    assert remote_head() == later_selected
    write(repo / docs / "repair-review.json", {"changed_after_later_selection": True})
    commit("Replace a historical repair review after a later selection")
    run("git", "push", "origin", "HEAD:refs/heads/tgd3d-vulkan", succeeds=False, contains="mismatched scoped repair evidence")
    assert remote_head() == later_selected
    print("PASS supporting repair evidence retained across a later baseline")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pwsh", default=shutil.which("pwsh"))
    args = parser.parse_args()
    if not args.pwsh:
        parser.error("Specify the PowerShell executable with --pwsh")
    with tempfile.TemporaryDirectory(prefix="graphics-promotion-test-") as directory:
        exercise(args.pwsh, Path(directory))
