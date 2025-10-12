#!/usr/bin/env python3
# scripts/gen_index.py
"""
Generate docs/index.acl and docs/pkgs/<name>/<ver>/manifest.acl from a pkg workspace.

Layout:
  pkgs/<name>/<version>/*.pkg

Outputs:
  docs/index.acl
  docs/pkgs/<name>/<version>/manifest.acl

Behavior:
  - Creates docs/ subdirs as needed.
  - Overwrites existing manifest.acl and index.acl.
  - SHA256 is computed as lowercase hex using the project's src/sha256.c (built and run).
  - Uses hardcoded release URL bases:
      Index base:  https://atlaslinux.github.io/pandora/
      Manifest pkg base: https://github.com/atlaslinux/pandora
"""
from __future__ import annotations
import argparse
import sys
import subprocess
import os
import re
from pathlib import Path
from typing import Dict, List, Tuple, Optional

# --- compute_sha256 replaced to compile and run ../src/sha256.c (which has main) ---
SCRIPT_DIR = Path(__file__).resolve().parent
SRC_SHA256_C = (SCRIPT_DIR / ".." / "src" / "sha256.c").resolve()
TOOLS_DIR = SCRIPT_DIR / "tools"
TOOL_BIN = (TOOLS_DIR / "sha256_c_bin").resolve()
CC = os.environ.get("CC", "cc")
CFLAGS = os.environ.get("CFLAGS", "-O2 -std=c11 -I" + str(SRC_SHA256_C.parent))

_INPUT_PKG_ROOT: Optional[Path] = None

def build_sha256_binary():
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    needs_build = True
    if TOOL_BIN.exists():
        bin_mtime = TOOL_BIN.stat().st_mtime
        try:
            src_mtime = SRC_SHA256_C.stat().st_mtime
        except FileNotFoundError:
            raise RuntimeError(f"Missing source SHA256 C file: {SRC_SHA256_C}")
        if src_mtime <= bin_mtime:
            needs_build = False
    if not needs_build:
        return TOOL_BIN
    cmd = [CC] + CFLAGS.split() + [str(SRC_SHA256_C), "-o", str(TOOL_BIN)]
    subprocess.check_call(cmd)
    TOOL_BIN.chmod(TOOL_BIN.stat().st_mode | 0o111)
    return TOOL_BIN

def _find_local_pkg_by_name(pkg_name: str) -> Optional[Path]:
    """Search the input pkg root for a file with basename == pkg_name and return its Path, or None."""
    global _INPUT_PKG_ROOT
    if not _INPUT_PKG_ROOT:
        return None
    for p in _INPUT_PKG_ROOT.rglob(pkg_name):
        if p.is_file():
            return p
    return None

def compute_sha256(path: Path) -> str:
    """
    Compute sha256 by compiling and running ../src/sha256.c (which contains main).
    If 'path' doesn't exist and looks like a URL or filename, attempt to locate the
    local package file (by basename) under the input directory before failing.
    """
    orig = path
    if isinstance(path, str):
        path = Path(path)
    # If the path exists locally, use it directly
    if not path.exists():
        path_str = str(orig)
        m = re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*://', path_str)
        basename = Path(path_str).name
        found = None
        if m:
            # looks like a URL: search by basename in input pkg tree
            found = _find_local_pkg_by_name(basename)
        else:
            # try resolving relative to script dir, then fallback to search by basename
            candidate = (SCRIPT_DIR / path).resolve()
            if candidate.exists():
                found = candidate
            else:
                found = _find_local_pkg_by_name(basename)
        if found is None:
            raise RuntimeError(f"Cannot find local package file for '{orig}'. Searched for basename '{basename}' under {_INPUT_PKG_ROOT}")
        path = found

    binpath = build_sha256_binary()
    proc = subprocess.run([str(binpath), str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"sha256 binary failed for {path}: rc={proc.returncode} stderr={proc.stderr.strip()}")
    hex = proc.stdout.strip()
    if len(hex) != 64:
        raise RuntimeError(f"sha256 binary output invalid for {path}: '{hex}'")
    return hex
# --- end replacement ---


def find_pkgs(input_dir: Path) -> Dict[str, List[Tuple[str, Path]]]:
    """
    Discover packages under input_dir.

    For each package name/version dir prefer the file named exactly: <name>-<version>.pkg.
    If that file does not exist, fall back to the first .pkg found in lexicographic order.
    """
    pkgs: Dict[str, List[Tuple[str, Path]]] = {}
    if not input_dir.exists():
        return pkgs
    for name_dir in sorted(input_dir.iterdir()):
        if not name_dir.is_dir():
            continue
        name = name_dir.name
        for ver_dir in sorted(name_dir.iterdir()):
            if not ver_dir.is_dir():
                continue
            version = ver_dir.name
            pkg_files = sorted([p for p in ver_dir.iterdir() if p.is_file() and p.suffix == ".pkg"])
            if not pkg_files:
                continue
            # Prefer exact filename "<name>-<version>.pkg"
            expected_name = f"{name}-{version}.pkg"
            exact = None
            for p in pkg_files:
                if p.name == expected_name:
                    exact = p
                    break
            pkg_path = exact if exact else pkg_files[0]
            pkgs.setdefault(name, []).append((version, pkg_path))
    return pkgs


def write_manifest(out_manifest: Path, name: str, version: str, sha256: str, pkg_url: str):
    out_manifest.parent.mkdir(parents=True, exist_ok=True)
    content = []
    content.append('Manifest {')
    content.append(f'    string name = "{name}";')
    content.append(f'    string version = "{version}";')
    content.append(f'    string sha256 = "{sha256}";')
    content.append(f'    string pkg_url = "{pkg_url}";')
    content.append('    bool signed = false;')
    content.append('}')
    out_manifest.write_text("\n".join(content) + "\n", encoding="utf-8")


def generate_index(out_dir: Path, pkgs: Dict[str, List[Tuple[str, Path]]]):
    """
    Generate index.acl where the Registry.url is fixed to:
      https://atlaslinux.github.io/pandora/index.acl

    Manifests will contain pkg_url values prefixed with:
      https://github.com/atlaslinux/pandora
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    index_lines: List[str] = []

    INDEX_BASE = "https://atlaslinux.github.io/pandora/"
    MANIFEST_PKG_BASE = "https://github.com/atlaslinux/pandora"

    # Open Registry block and include metadata
    index_lines.append("Registry {")
    index_lines.append(f'    string url = "{INDEX_BASE}index.acl";')
    index_lines.append("    int priority = 100;")
    index_lines.append("    bool require_signatures = false;")
    index_lines.append('    string cache_policy = "ttl=3600";')
    index_lines.append("")  # blank line inside Registry for readability

    # For deterministic output, iterate sorted names and versions (versions sorted reverse lexicographic)
    for name in sorted(pkgs.keys()):
        versions = sorted([v for v, p in pkgs[name]], reverse=True)
        index_lines.append(f'    /* {name} package */')
        index_lines.append(f'    Package "{name}" {{')
        versions_list = ", ".join(f'"{v}"' for v in versions)
        index_lines.append(f'        string[] versions = {{ {versions_list} }};')
        index_lines.append(f'        string latest = "{versions[0]}";')
        index_lines.append('        string pkg_base_url = "";')
        index_lines.append('')
        # include Version sub-blocks
        for (ver, pkgpath) in sorted(pkgs[name], key=lambda x: x[0], reverse=True):
            sha = compute_sha256(pkgpath)
            manifest_rel = f'pkgs/{name}/{ver}/manifest.acl'
            manifest_url = INDEX_BASE + manifest_rel
            pkg_url = f'{MANIFEST_PKG_BASE}/releases/download/{name}-{ver}/{pkgpath.name}'
            index_lines.append(f'        Version "{ver}" {{')
            index_lines.append(f'            string manifest_url = "{manifest_url}";')
            index_lines.append(f'            string pkg_url = "{pkg_url}";')
            index_lines.append(f'            string sha256 = "{sha}";')
            index_lines.append('            bool deprecated = false;')
            index_lines.append('        }')
            index_lines.append('')
        index_lines.append('    }')
        index_lines.append('')  # blank after each Package

    # Close Registry block
    index_lines.append("}")

    index_path = out_dir / "index.acl"
    index_path.write_text("\n".join(index_lines) + "\n", encoding="utf-8")
    return index_path


def main(argv):
    ap = argparse.ArgumentParser(description="Generate index.acl and manifests from pkgs workspace")
    ap.add_argument("--input", "-i", required=True, help="pkgs input dir")
    ap.add_argument("--out", "-o", required=True, help="docs output dir")
    args = ap.parse_args(argv)

    input_dir = Path(args.input)
    out_dir = Path(args.out)

    # allow compute_sha256 to search the workspace when given URLs
    global _INPUT_PKG_ROOT
    _INPUT_PKG_ROOT = input_dir.resolve()

    pkgs = find_pkgs(input_dir)
    if not pkgs:
        print("No packages found under", input_dir, file=sys.stderr)
        sys.exit(1)

    # Write manifests and compute SHAs (manifests live under docs/pkgs/...)
    MANIFEST_PKG_BASE = "https://github.com/atlaslinux/pandora"
    INDEX_BASE = "https://atlaslinux.github.io/pandora/"

    for name, entries in pkgs.items():
        for ver, pkgpath in entries:
            sha = compute_sha256(pkgpath)
            manifest_path = out_dir / "pkgs" / name / ver / "manifest.acl"
            pkg_url = f'{MANIFEST_PKG_BASE}/releases/download/{name}-{ver}/{pkgpath.name}'
            write_manifest(manifest_path, name, ver, sha, pkg_url)

    # Generate index.acl with Package blocks inside Registry
    index_path = generate_index(out_dir, pkgs)
    print("Wrote index:", index_path)
    for p in sorted(out_dir.rglob("*")):
        if p.is_file():
            print("WROTE", p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
