#!/usr/bin/env python3
# scripts/create_package.py
"""
Create a deterministic .pkg from a package source dir, using local `arch` if available.
If `arch` is missing, this script will attempt to compile src/arch.c into a local arch binary.

Usage:
  scripts/create_package.py <src_dir> [--out-root pkgs] [--keep-temp] [--arch-cmd PATH]

Examples:
  scripts/create_package.py pkgs/src/mytool/3.0.0 --out-root pkgs

Behavior:
- Expects a manifest.acl file at the top of <src_dir>.
- Produces <out_root>/<name>/<ver>/<name>-<ver>.pkg
- If 'arch' executable is found in PATH or provided via --arch-cmd, calls: arch <src_dir> <out_pkg>
- Otherwise attempts to compile src/arch.c to ./build/arch and use it; if compilation fails, falls back to deterministic tar.gz.
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
    # replace existing archive_sha or archive_sha256 line if present
    pat = re.compile(r'^\s*(string\s+)?archive_sha(?:256)?\s*=\s*".*"\s*;\s*$', re.MULTILINE)
    if pat.search(text):
        text = pat.sub(new_line, text, count=1)
    else:
        # attempt to insert before closing brace of first top-level block
        m = re.search(r'^[a-zA-Z_][a-zA-Z0-9_]*\s*\{', text, re.MULTILINE)
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

# -------- arch build / invocation --------

def try_find_arch(provided: Optional[str]) -> Optional[Path]:
    # If user provided explicit arch-cmd, prefer that (verify executable)
    if provided:
        p = Path(provided)
        if p.is_file() and os.access(p, os.X_OK):
            return p
        # maybe it's in PATH; shutil.which handles that
        which = shutil.which(provided)
        if which:
            return Path(which)
        return None
    # look in PATH
    which = shutil.which("arch")
    if which:
        return Path(which)
    # try local build/arch
    local = Path("build") / "arch"
    if local.is_file() and os.access(local, os.X_OK):
        return local
    # try .local/bin/arch
    local2 = Path(".local") / "bin" / "arch"
    if local2.is_file() and os.access(local2, os.X_OK):
        return local2
    return None

def attempt_build_arch(src_dir: Path) -> Optional[Path]:
    """
    Try to compile src/arch.c into build/arch (or .local/bin/arch).
    Returns Path to compiled arch on success, or None on failure.
    """
    src_c = Path("src") / "arch.c"
    if not src_c.is_file():
        return None
    out_dir = Path("build")
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "arch"
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return None
    cmd = [cc, "-O2", "-std=c11", "-o", str(out_path), str(src_c)]
    print("Compiling arch:", " ".join(cmd))
    try:
        res = subprocess.run(cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            print("Compilation failed:", res.stderr.strip(), file=sys.stderr)
            return None
        # ensure executable bit
        out_path.chmod(0o755)
        return out_path
    except Exception as e:
        print("Compilation error:", e, file=sys.stderr)
        return None

def call_arch(arch_path: Path, src: Path, out_pkg: Path) -> int:
    cmd = [str(arch_path), "pack", str(out_pkg), str(src)]
    print("Running arch:", " ".join(cmd))
    try:
        res = subprocess.run(cmd, check=False)
        return res.returncode
    except Exception as e:
        print("arch invocation failed:", e, file=sys.stderr)
        return 1

# -------- tar fallback creation --------

def create_tar_fallback(src: Path, out_pkg: Path) -> int:
    tmpdir = tempfile.mkdtemp(prefix="create_pkg_tmp_")
    try:
        tmp_tar = Path(tmpdir) / "archive.tar"
        # Use a shell pipeline to create deterministic tar. Keep it simple and rely on GNU tar and gzip.
        # Build a sorted list of files and feed to tar with --null -T - and deterministic options.
        # The --transform 's|^\./||' removes leading ./ entries so archive root contains package root contents.
        shell_cmd = (
            f"cd {str(src)!r} && "
            "find . -print0 | LC_ALL=C sort -z | "
            f"tar --null -T - --numeric-owner --owner=0 --group=0 --mtime='UTC 1970-01-01' -cf {str(tmp_tar)!r} --transform 's|^\\./||'"
        )
        res = subprocess.run(shell_cmd, shell=True)
        if res.returncode != 0:
            print("tar creation failed", file=sys.stderr)
            return res.returncode
        tmp_gz = Path(tmpdir) / "archive.tar.gz"
        gz_cmd = ["gzip", "-9", "-n", "-c", str(tmp_tar)]
        with tmp_gz.open("wb") as outf:
            proc = subprocess.Popen(gz_cmd, stdout=outf)
            proc.wait()
            if proc.returncode != 0:
                print("gzip failed", file=sys.stderr)
                return proc.returncode
        ensure_parent(out_pkg)
        shutil.move(str(tmp_gz), str(out_pkg))
        return 0
    finally:
        try:
            shutil.rmtree(tmpdir)
        except Exception:
            pass

# -------- main flow --------

def main(argv):
    ap = argparse.ArgumentParser(description="Create package .pkg from source tree, auto-building arch if needed")
    ap.add_argument("src_dir", help="source directory: docs/src/<name>/<ver>")
    ap.add_argument("--out-root", default="pkgs", help="output root (default: pkgs)")
    ap.add_argument("--keep-temp", action="store_true", help="keep temporary files on error")
    ap.add_argument("--arch-cmd", default=None, help="custom arch command path (optional)")
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

    # determine arch binary
    arch_path = try_find_arch(args.arch_cmd)
    if not arch_path:
        print("arch binary not found. Attempting to build from src/arch.c ...")
        built = attempt_build_arch(Path("src"))
        if built:
            arch_path = built
            print("Built arch at", arch_path)
        else:
            print("Could not build arch; will use tar fallback", file=sys.stderr)

    # try arch if available
    if arch_path:
        rc = call_arch(arch_path, src, out_pkg)
        if rc == 0 and out_pkg.is_file():
            print("arch succeeded, produced:", out_pkg)
        else:
            print(f"arch returned code {rc}; falling back to deterministic tar", file=sys.stderr)
            rc2 = create_tar_fallback(src, out_pkg)
            if rc2 != 0:
                print("Failed to produce archive via fallback (code {})".format(rc2), file=sys.stderr)
                return 1
            print("Created archive via tar fallback:", out_pkg)
    else:
        rc2 = create_tar_fallback(src, out_pkg)
        if rc2 != 0:
            print("Failed to produce archive via fallback (code {})".format(rc2), file=sys.stderr)
            return 1
        print("Created archive via tar fallback:", out_pkg)

    # compute sha256
    sha = compute_sha256(out_pkg)
    print("SHA256:", sha)

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
