#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# switch-graphics-baseline-pre-push
"""Check the exact Git tree when a Switch baseline, fault register or its evidence changes."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile

LEDGER = "tegra-x1-wddm/docs/open-faults.json"
BASELINE = "tegra-x1-wddm/docs/graphics-baseline.json"
GATE = "tegra-x1-wddm/tools/Select-GraphicsBaseline.ps1"
LIBRARY = "tegra-x1-wddm/tools/tgd3d-default/Baseline-Promotion.ps1"
CHECKER = "tegra-x1-wddm/tools/check_graphics_baseline_push.py"
CONTRACT_CHECKER = "tegra-x1-wddm/tools/check_dxgkrnl_contracts.py"
MARKER = b"# switch-graphics-baseline-pre-push\n"


def git(*args, cwd=None, check=True, env=None):
    return subprocess.run(["git", *args], cwd=cwd, check=check, env=env, capture_output=True, text=True)

def blob(commit, path):
    return subprocess.check_output(["git", "show", f"{commit}:{path}"])


def verifier_bytes(commit):
    return {path: blob(commit, path) for path in (GATE, LIBRARY)}


def verifier_id(files):
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    return hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()


def hook_directory():
    return Path(git("rev-parse", "--path-format=absolute", "--git-path", "hooks/pre-push").stdout.strip()).parent


def install():
    target = hook_directory() / "pre-push"
    data = blob("HEAD", CHECKER)
    root = Path(git("rev-parse", "--show-toplevel").stdout.strip())
    for path in (GATE, LIBRARY, CHECKER):
        accepted = git("rev-parse", f"HEAD:{path}").stdout.strip()
        if git("hash-object", f"--path={path}", str(root / path)).stdout.strip() != accepted:
            raise RuntimeError("Install the verifier from a committed, reviewed revision.")
    if target.is_symlink() or (target.exists() and MARKER not in target.read_bytes().splitlines(keepends=True)[:4]):
        raise RuntimeError(f"Existing pre-push hook needs to call this checker: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    files = verifier_bytes("HEAD")
    revision = verifier_id(files)
    store = target.parent / "switch-graphics-verifiers"
    version = store / revision
    for name, content in files.items():
        destination = version / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and destination.read_bytes() != content:
            raise RuntimeError("Previously installed verifier bytes changed")
        destination.write_bytes(content)
    with tempfile.NamedTemporaryFile(dir=store, delete=False) as stream:
        stream.write(revision.encode())
        pointer = Path(stream.name)
    os.replace(pointer, store / "active")
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.chmod(0o755)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Installed Switch baseline pre-push check at {target}")


def changed(before, after, path):
    comparison = git("diff", "--quiet", before, after, "--", path, check=False)
    if comparison.returncode not in (0, 1):
        raise RuntimeError("Cannot compare the remote baseline. Fetch the destination history first.")
    return comparison.returncode == 1


def require_forward_baseline(previous, current):
    if not git("ls-tree", "--name-only", previous, "--", BASELINE).stdout.strip():
        return
    old = json.loads(blob(previous, BASELINE))["source_commit"]
    new = json.loads(blob(current, BASELINE))["source_commit"]
    if old == new or git("merge-base", "--is-ancestor", old, new, check=False).returncode:
        raise RuntimeError("A changed baseline must advance from the accepted source on the destination branch.")


def check_tree(commit, actions, trusted):
    config = git("config", "--path", "--get", "switch.graphicsGatePwsh", check=False).stdout.strip()
    shell = config or shutil.which("pwsh") or shutil.which("powershell.exe")
    if not shell:
        raise RuntimeError("PowerShell is required to push a changed Switch baseline or fault ledger.")
    objects = Path(git("rev-parse", "--path-format=absolute", "--git-path", "objects").stdout.strip())
    shallow = Path(git("rev-parse", "--path-format=absolute", "--git-path", "shallow").stdout.strip())
    object_format = git("rev-parse", "--show-object-format").stdout.strip()
    clean = os.environ.copy()
    for variable in git("rev-parse", "--local-env-vars").stdout.splitlines():
        clean.pop(variable, None)
    with tempfile.TemporaryDirectory(prefix="switch-baseline-push-") as temporary:
        view = Path(temporary) / "view"
        # Cloning a shallow promisor repository can repack unrelated, unmaterialized
        # objects. Borrow its object directory and its exact history boundary instead.
        git("init", "--quiet", f"--object-format={object_format}", str(view), env=clean)
        metadata = view / ".git"
        (metadata / "objects/info/alternates").write_bytes((objects.as_posix() + "\n").encode())
        if shallow.is_file():
            shutil.copyfile(shallow, metadata / "shallow")
        git("config", "core.autocrlf", "false", cwd=view, env=clean)
        git("sparse-checkout", "set", "--no-cone", "/tegra-x1-wddm/docs/", cwd=view, env=clean)
        git("checkout", "--quiet", "--detach", commit, cwd=view, env=clean)
        verifier = Path(temporary) / "verifier"
        for name, content in trusted.items():
            destination = verifier / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        for action in actions:
            subprocess.run([shell, "-NoProfile", "-File", str(verifier / GATE), "-Action", action,
                            "-RepositoryRoot", str(view)],
                           cwd=view, env=clean, check=True)
        ledger = json.loads((view / LEDGER).read_bytes())
        if "dxgkrnl_contracts" in ledger:
            contract_checker = view / CONTRACT_CHECKER
            contract_checker.parent.mkdir(parents=True, exist_ok=True)
            contract_checker.write_bytes(blob(commit, CONTRACT_CHECKER))
            subprocess.run([sys.executable, str(contract_checker)], cwd=view, env=clean, check=True)

def trusted_verifier(remote):
    store = hook_directory() / "switch-graphics-verifiers"
    pointer = store / "active"
    if pointer.is_file():
        revision = pointer.read_text().strip()
        if len(revision) != 64 or any(ch not in "0123456789abcdef" for ch in revision):
            raise RuntimeError("Invalid locally installed policy verifier")
        files = {name: (store / revision / name).read_bytes() for name in (GATE, LIBRARY)}
        if verifier_id(files) != revision:
            raise RuntimeError("Installed baseline verifier changed")
        return files
    if remote.strip("0"):
        return verifier_bytes(remote)
    raise RuntimeError("Install the reviewed baseline verifier before creating the protected branch.")


def retained_reports(commit):
    """Find reports referenced by the preceding accepted tree without running its code."""
    ledger = json.loads(blob(commit, LEDGER))
    selected = json.loads(blob(commit, BASELINE))
    qualification = selected["qualification"]
    paths = {qualification}
    if selected.get("qualification_sha256"):
        data = json.loads(blob(commit, qualification))
        reports = data["cases"] + data.get("baseline_cases", []) + data["system_log"]["restarts"]
        if data.get("change_kind") == "verified-fix":
            reports += (data.get("verified_fix") or {}).get("evidence") or []
        paths.update(str(PurePosixPath(qualification).parent / row["evidence_file"]) for row in reports)
    for comparison in ledger.get("baseline_comparisons", []):
        earlier = comparison["before_commit"]
        previous = json.loads(blob(earlier, BASELINE))["qualification"]
        record = json.loads(blob(earlier, previous))
        paths.add(previous)
        controls = [row for row in record["cases"]
                    if row["evidence_sha256"].lower() == comparison["previous_control_sha256"].lower()]
        if len(controls) != 1:
            raise RuntimeError("The earlier baseline must identify one working comparison control.")
        paths.add(str(PurePosixPath(previous).parent / controls[0]["evidence_file"]))
    for fault in ledger["faults"]:
        paths.update(ref.split("#", 1)[0] for ref in fault["evidence"])
        repair = fault.get("verified_fix")
        if repair:
            paths.update(repair[field] for field in ("causal_evidence_file", "code_review_file", "build_file"))
        if fault["status"] == "closed":
            fix = fault["resolution"]
            paths.add(fix["causal_evidence_file"])
            paths.update(row["evidence_file"] for row in fix["original_scenario_runs"])
    if "dxgkrnl_contracts" in ledger:
        contracts = ledger["dxgkrnl_contracts"]
        docs = PurePosixPath(LEDGER).parent
        paths.add(str(docs / contracts["catalog"]))
        for row in contracts["rounds"]:
            paths.add(str(docs / row["declaration"]))
            if row["state"] == "recorded":
                paths.add(str(docs / row["receipt"]))
        paths.update(str(docs / row["receipt"]) for row in contracts.get("unobserved_recovery_boots", []))
    return paths


def check_updates(lines):
    for line in lines:
        _, local, remote_ref, remote = line.split()
        if remote_ref != "refs/heads/tgd3d-vulkan":
            continue
        if not local.strip("0"):
            raise RuntimeError("Deleting the branch would remove the accepted Switch baseline.")
        if not remote.strip("0"):
            check_tree(local, ["CheckLedger", "CheckPublication"], trusted_verifier(remote))
            continue
        ancestry = git("merge-base", "--is-ancestor", remote, local, check=False)
        if ancestry.returncode:
            raise RuntimeError("Cannot rewrite the accepted baseline and fault history. Fetch and integrate the destination branch first.")
        if changed(remote, local, BASELINE):
            require_forward_baseline(remote, local)
        commits = git("log", "--reverse", "--topo-order", "--full-history", "--format=%H",
                      f"{remote}..{local}", "--", "tegra-x1-wddm/docs", CONTRACT_CHECKER).stdout.splitlines()
        trusted = None
        for commit in dict.fromkeys(commits):
            parents = git("rev-list", "--parents", "-n", "1", commit).stdout.split()[1:]
            # A merge selects a new baseline only if it changes the receiving branch.
            # New side-branch transitions are visited as commits in their own right.
            baseline_changed = not parents or changed(parents[0], commit, BASELINE)
            receiving_parents = [parent for parent in parents
                                 if not git("merge-base", "--is-ancestor", remote, parent, check=False).returncode]
            for parent in receiving_parents:
                if changed(parent, commit, BASELINE):
                    require_forward_baseline(parent, commit)
                    if parents[0] not in receiving_parents:
                        baseline_changed = True
            if not baseline_changed and not any(changed(parent, commit, LEDGER) for parent in parents):
                touched = set()
                for parent in parents:
                    files = git("diff", "--name-only", "--no-renames", parent, commit, "--", "tegra-x1-wddm/docs")
                    touched.update(set(files.stdout.splitlines()) & retained_reports(parent))
                    if changed(parent, commit, CONTRACT_CHECKER):
                        touched.add(CONTRACT_CHECKER)
                if not touched:
                    continue
            actions = ["CheckLedger"]
            if baseline_changed:
                actions.append("CheckPublication")
            if trusted is None:
                trusted = trusted_verifier(remote)
            check_tree(commit, actions, trusted)


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--install"]:
            install()
        else:
            check_updates(sys.stdin)
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Switch baseline push rejected. {error}", file=sys.stderr)
        sys.exit(1)
