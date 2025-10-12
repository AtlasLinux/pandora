#!/usr/bin/env python3
# scripts/create_package.py
"""
Create a deterministic .pkg from a package source dir, always using local build/arch.

Usage:
  scripts/create_package.py <src_dir> [--out-root pkgs] [--keep-temp]

Behavior:
- Expects a manifest.acl file at the top of <src_dir>.
- Produces <out_root>/<name>/<ver>/<name>-<ver>.pkg
- ALWAYS uses build/arch. If build/arch is missing, attempts to compile src/arch.c -> build/arch.
- If compilation or arch invocation fails, the script exits with non-zero (no tar fallback).
- Computes SHA256 and updates (or inserts) archive_sha256 in manifest.acl.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

# -------- utility functions --------

def compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def find_manifest(src: Path) -> Path:
    m = src / "manifest.acl"
    if m.is_file():
        return m
    raise FileNotFoundError(f"manifest.acl not found in {src}")

def update_manifest_archive_sha(manifest_path: Path, sha_hex: str) -> None:
    text = manifest_path.read_text(encoding="utf-8")
    new_line = f'    string archive_sha256 = "{sha_hex}";'
    pat = re.compile(r'^\s*(string\s+)?archive_sha(?:256)?\s*=\s*".*"\s*;\s*$', re.MULTILINE)
    if pat.search(text):
        text = pat.sub(new_line, text, count=1)
    else:
        # insert before the first top-level closing brace if possible
        m = re.search(r'^[A-Za-z_][A-Za-z0-9_]*\s*\{', text, re.MULTILINE)
        if m:
            start = m.end()
            depth = 1
            i = start
            while i < len(text) and depth > 0:
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                i += 1
            insert_at = i - 1 if depth == 0 else len(text)
            insert_text = "\n    " + new_line.strip() + "\n"
            text = text[:insert_at] + insert_text + text[insert_at:]
        else:
            if not text.endswith("\n"):
                text += "\n"
            text += new_line + "\n"
    manifest_path.write_text(text, encoding="utf-8")

def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

# -------- arch build / invocation (always use build/arch) --------

def arch_executable() -> Path:
    """
    Return the Path to build/arch. If missing, attempt to compile src/arch.c into it.
    If compilation fails, raise RuntimeError.
    """
    out_path = Path("build") / "arch"
    if out_path.is_file() and os.access(out_path, os.X_OK):
        return out_path

    # attempt to build from src/arch.c
    src_c = Path("src") / "arch.c"
    if not src_c.is_file():
        raise RuntimeError("build/arch not found and src/arch.c missing; cannot proceed")

    out_dir = out_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    # prefer cc/gcc/clang
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise RuntimeError("C compiler not found; cannot build build/arch")

    cmd = [cc, "-O2", "-std=c11", "-o", str(out_path), str(src_c)]
    print("Compiling arch:", " ".join(cmd), file=sys.stderr)
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print("Compilation of build/arch failed.", file=sys.stderr)
        print("=== stdout ===", file=sys.stderr)
        print(res.stdout, file=sys.stderr)
        print("=== stderr ===", file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        raise RuntimeError("compilation failed for build/arch")
    out_path.chmod(0o755)
    return out_path

def call_arch(arch_path: Path, src: Path, out_pkg: Path) -> int:
    cmd = [str(arch_path), "pack", str(out_pkg), str(src)]
    print("Running arch:", " ".join(cmd), file=sys.stderr)
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # print stdout/stderr for CI visibility
        if res.stdout:
            print("arch stdout:", res.stdout, file=sys.stderr)
        if res.stderr:
            print("arch stderr:", res.stderr, file=sys.stderr)
        return res.returncode
    except Exception as e:
        print("arch invocation failed:", e, file=sys.stderr)
        return 1

# -------- main flow --------

def main(argv):
    ap = argparse.ArgumentParser(description="Create package .pkg from source tree; always use build/arch")
    ap.add_argument("src_dir", help="source directory: pkgs/src/<name>/<ver>")
    ap.add_argument("--out-root", default="pkgs", help="output root (default: pkgs)")
    ap.add_argument("--keep-temp", action="store_true", help="keep temporary files on error")
    args = ap.parse_args(argv)

    src = Path(args.src_dir).resolve()
    if not src.is_dir():
        print("Source directory not found:", src, file=sys.stderr)
        return 2

    # infer name/version: expect src like .../<name>/<version>
    if src.parent is None:
        print("Source path too short to infer name/version", file=sys.stderr)
        return 2
    name = src.parent.name
    version = src.name
    if not name or not version:
        print("Could not determine name/version from path", src, file=sys.stderr)
        return 2

    try:
        manifest_path = find_manifest(src)
    except FileNotFoundError as e:
        print(e, file=sys.stderr)
        return 2

    out_root = Path(args.out_root)
    out_pkg_dir = out_root / name / version
    out_pkg_dir.mkdir(parents=True, exist_ok=True)
    out_pkg = out_pkg_dir / f"{name}-{version}.pkg"

    # ensure parent dir exists
    ensure_parent(out_pkg)

    # Always use build/arch. Try to find or compile it.
    try:
        arch_path = arch_executable()
    except Exception as e:
        print("ERROR: could not obtain build/arch:", e, file=sys.stderr)
        return 3

    rc = call_arch(arch_path, src, out_pkg)
    if rc != 0 or not out_pkg.is_file():
        print(f"ERROR: build/arch failed (rc={rc}) or did not produce {out_pkg}", file=sys.stderr)
        return 4

    print("arch succeeded, produced:", out_pkg, file=sys.stderr)

    # compute sha256
    try:
        sha = compute_sha256(out_pkg)
        print("SHA256:", sha)
    except Exception as e:
        print("ERROR computing SHA256:", e, file=sys.stderr)
        return 5

    # update manifest archive_sha256
    try:
        update_manifest_archive_sha(manifest_path, sha)
        print("Updated manifest archive_sha256 in", manifest_path)
    except Exception as e:
        print("Warning: failed to update manifest:", e, file=sys.stderr)

    print("Wrote package:", out_pkg)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
