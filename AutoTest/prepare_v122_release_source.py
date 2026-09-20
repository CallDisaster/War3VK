"""Freeze tracked working bytes and pinned submodules into a NEW build source.

No build, dependency update, checkout, deletion or deployment is performed.
Dirty submodule working files are never used. The manifest is written last.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import zipfile
from pathlib import Path, PurePosixPath


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *args])


def relative(value: str) -> str:
    p = PurePosixPath(value)
    if p.is_absolute() or any(x in ("", ".", "..") for x in value.split("/")):
        raise ValueError(f"Unsafe archive path: {value!r}")
    if "\\" in value or ":" in value:
        raise ValueError(f"Unsafe Windows path: {value!r}")
    return str(p)


def identity(path: Path) -> dict:
    data = path.read_bytes()
    return {"size": len(data), "sha256": hashlib.sha256(data).hexdigest().upper()}


def freeze(root: Path, out: Path, extra: list[str]) -> dict:
    root, out = root.resolve(), out.resolve()
    if out == root or root in out.parents:
        raise ValueError("Build snapshot must be outside the source repository")
    if out.exists():
        raise FileExistsError(out)
    status = git(root, "status", "--porcelain=v1", "-uall").decode("utf-8")
    head = git(root, "rev-parse", "HEAD").decode().strip()
    entries = git(root, "ls-files", "--stage", "-z").split(b"\0")
    out.mkdir(parents=True, exist_ok=False)
    source = out / "source"
    source.mkdir()
    files, dependencies = {}, {}

    def copy_file(src: Path, dst: Path) -> None:
        if not src.is_file() or src.is_symlink():
            raise ValueError(f"Not a regular source file: {src}")
        before = identity(src)
        dst.parent.mkdir(parents=True, exist_ok=True)
        with src.open("rb") as reader, dst.open("xb") as writer:
            shutil.copyfileobj(reader, writer)
        if identity(src) != before or identity(dst) != before:
            raise RuntimeError(f"Source changed while freezing: {src}")
        files[dst.relative_to(source).as_posix()] = before

    for raw in entries:
        if not raw:
            continue
        meta, name = raw.split(b"\t", 1)
        mode, oid, stage = meta.decode().split()
        rel = relative(name.decode("utf-8"))
        if stage != "0":
            raise ValueError(f"Unmerged index entry: {rel}")
        if mode == "160000":
            archive = out / (rel.replace("/", "_") + ".zip")
            subprocess.run(["git", "-C", str(root / rel), "archive", "--format=zip",
                            "--output=" + str(archive), oid], check=True)
            dependencies[rel] = {"commit": oid, "archive": identity(archive)}
            with zipfile.ZipFile(archive) as z:
                for member in z.infolist():
                    if member.is_dir():
                        continue
                    if ((member.external_attr >> 16) & 0o170000) == 0o120000:
                        raise ValueError("Symlink in dependency archive")
                    dest = source / rel / relative(member.filename)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(member) as reader, dest.open("xb") as writer:
                        shutil.copyfileobj(reader, writer)
                    files[dest.relative_to(source).as_posix()] = identity(dest)
        elif mode in ("100644", "100755"):
            copy_file(root / rel, source / rel)
        else:
            raise ValueError(f"Unsupported tracked mode: {mode} {rel}")

    # Official project shim, not the untracked copy in the upstream worktree.
    copy_file(root / "subprojects/packagefiles/imgui/meson.build",
              source / "subprojects/imgui/meson.build")
    # Explicit additions only: never absorb unrelated untracked experiments.
    for addition in ["AutoTest/prepare_v122_release_source.py", *extra]:
        addition = relative(addition)
        if addition not in files:
            copy_file(root / addition, source / addition)
    if git(root, "status", "--porcelain=v1", "-uall").decode("utf-8") != status:
        raise RuntimeError("Source status changed during freeze")
    for rel, expected in files.items():
        if identity(source / rel) != expected:
            raise RuntimeError(f"Snapshot mismatch: {rel}")
    result = {"sourceRoot": str(root), "sourceHead": head, "sourceStatus": status,
              "snapshot": str(source), "dependencies": dependencies, "files": files,
              "releaseAccepted": False}
    with (out / "source-manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    return result


def refresh(root: Path, out: Path, additions: list[str],
            previous: str = 'source-manifest.json', final_name: str = 'source-final.json') -> dict:
    """Refresh a stopped build's generated source mirror; retain both manifests.

    Only files present in the first manifest and explicit additions are copied.
    Dependencies remain at the archived commits. Never use during a build.
    """
    root, out = root.resolve(), out.resolve()
    initial = json.loads((out / relative(previous)).read_text(encoding='utf-8'))
    source = (out / 'source').resolve()
    if str(source) != initial['snapshot'] or str(root) != initial['sourceRoot']:
        raise ValueError('Frozen source location mismatch')
    final = out / relative(final_name)
    if final.exists():
        raise FileExistsError(final)
    files = dict(initial['files'])
    changes = {}
    for rel, expected in files.items():
        if identity(source / rel) != expected:
            raise ValueError(f'Unaccounted snapshot modification: {rel}')
    dependency_roots = tuple(x + '/' for x in initial['dependencies'])
    for rel in [*files, *(relative(x) for x in additions)]:
        if rel.startswith(dependency_roots):
            continue
        origin, dest = root / rel, source / rel
        if not origin.is_file() or origin.is_symlink():
            raise ValueError(f'Invalid source input: {rel}')
        now = identity(origin)
        if files.get(rel) != now:
            changes[rel] = {'before': files.get(rel), 'after': now}
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(origin, dest)
        files[rel] = now
    for rel, expected in files.items():
        if identity(source / rel) != expected:
            raise ValueError(f'Final source mismatch: {rel}')
    result = dict(initial, files=files, changesSinceInitial=changes,
                  sourceStatus=git(root, 'status', '--porcelain=v1', '-uall').decode('utf-8'))
    with final.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--refresh-stopped-build", action="store_true")
    parser.add_argument("--previous-manifest", default="source-manifest.json")
    parser.add_argument("--final-manifest", default="source-final.json")
    args = parser.parse_args()
    if args.refresh_stopped_build:
        result = refresh(args.root, args.out, args.include, args.previous_manifest, args.final_manifest)
    else:
        result = freeze(args.root, args.out, args.include)
    print(json.dumps({"snapshot": result["snapshot"], "files": len(result["files"]),
                      "dependencies": list(result["dependencies"]), "ok": True}))
