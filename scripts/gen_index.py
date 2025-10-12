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
  - SHA256 is computed as lowercase hex.
"""
from __future__ import annotations
import argparse
import hashlib
import sys
from pathlib import Path
from typing import Dict, List, Tuple

def compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(64 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def find_pkgs(input_dir: Path) -> Dict[str, List[Tuple[str, Path]]]:
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
            pkg_path = pkg_files[0]
            pkgs.setdefault(name, []).append((version, pkg_path))
    return pkgs

def write_manifest(out_manifest: Path, name: str, version: str, sha256: str, pkg_url: str):
    out_manifest.parent.mkdir(parents=True, exist_ok=True)
    content = []
    content.append('NAME {')
    content.append(f'    string name = "{name}";')
    content.append(f'    string version = "{version}";')
    content.append(f'    string sha256 = "{sha256}";')
    content.append(f'    string pkg_url = "{pkg_url}";')
    content.append('    bool signed = false;')
    content.append('}')
    out_manifest.write_text("\n".join(content) + "\n", encoding="utf-8")

def generate_index(out_dir: Path, pkgs: Dict[str, List[Tuple[str, Path]]], release_url_fn):
    out_dir.mkdir(parents=True, exist_ok=True)
    index_lines: List[str] = []

    # Open Registry block and include metadata
    index_lines.append("Registry {")
    index_lines.append(f'    string url = "{release_url_fn("index.acl")}";')
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
            manifest_url = release_url_fn(manifest_rel)
            pkg_url = release_url_fn(f'releases/download/{name}-{ver}/{pkgpath.name}')
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
    ap.add_argument("--pages-base", default=None, help="Base URL for Pages (e.g. https://user.github.io/repo). If omitted, emits relative paths")
    args = ap.parse_args(argv)

    input_dir = Path(args.input)
    out_dir = Path(args.out)
    pages_base = args.pages_base.rstrip("/") if args.pages_base else None

    pkgs = find_pkgs(input_dir)
    if not pkgs:
        print("No packages found under", input_dir, file=sys.stderr)
        sys.exit(1)

    def release_url_fn(relpath: str) -> str:
        if pages_base:
            return f"{pages_base.rstrip('/')}/{relpath.lstrip('/')}"
        return relpath

    # Write manifests and compute SHAs (manifests live under docs/pkgs/...)
    for name, entries in pkgs.items():
        for ver, pkgpath in entries:
            sha = compute_sha256(pkgpath)
            manifest_path = out_dir / "pkgs" / name / ver / "manifest.acl"
            pkg_url = release_url_fn(f"releases/download/{name}-{ver}/{pkgpath.name}")
            write_manifest(manifest_path, name, ver, sha, pkg_url)

    # Generate index.acl with Package blocks inside Registry
    index_path = generate_index(out_dir, pkgs, release_url_fn)
    print("Wrote index:", index_path)
    for p in sorted(out_dir.rglob("*")):
        if p.is_file():
            print("WROTE", p)
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
