# Pandora — design spec

*Single-user, home-only package manager: immutable store, symlink forests, atomic updates, exact-version deps, required automatic signatures. No code here — just a tight implementation-ready spec.*

---

## Table of contents

1. Goals & constraints
2. High-level architecture
3. On-disk layout (filesystem)
4. Manifest format
5. DB on-disk format (precise spec)
6. DB operations (semantics & algorithms)
7. Activation / forest rebuild / rollback (step-by-step)
8. Dependency resolution (exact pairs)
9. Signature & verification policy
10. Locking, atomicity, crash recovery
11. Housekeeping: GC & compaction
12. CLI surface & UX semantics
13. Example lifecycle (install → activate → dependent install → activate)
14. Performance & safety notes
15. Roadmap / next steps

---

## 1 — Goals & constraints

* Single-user (everything under `~/.pandora`). No root required.
* Only glibc on base system (you provide `curl` and `arch`).
* Required features:

  * **Local installs** (per-user store)
  * **Symlink forests** (profiles/views built from manifests)
  * **Atomic updates** (rename-based swaps + rollback)
  * **Exact version dependency resolution** (name + version pairs)
  * **Signatures required and verified automatically** during install
* DB = simple on-disk **hash map**. No external DB libs.

---

## 2 — High-level architecture

* **Store**: immutable package directories
  `~/.pandora/packages/<name>/<version>/…` (files + `.manifest` + optional `PKGINFO`)
* **Profiles / Forests**: symlink trees used by runtime:
  `~/.pandora/profiles/<profile>/…` and a convenience symlink `~/.pandora/latest -> profiles/default`
* **DB**: single file `~/.pandora/packages.db`, little-endian, fixed typed fields; stores all installed package entries, deps, offsets into a string pool. Implemented as an in-memory load / modify / rewrite model with atomic write-back.
* **CLI**: orchestrates install, activate, remove, list, rebuild, rollback, gc.
* **Locking**: single lockfile (flock) to serialize mutations.

---

## 3 — On-disk layout (filesystem)

```
~/.pandora/
  packages/                     # store
    foo/
      1.0/
        bin/foo
        lib/libfoo.so
        .manifest
        PKGINFO
    bar/
      2.3/
        ...
  profiles/
    default/                    # symlink forest (profile)
      bin/
        foo -> ../../packages/foo/1.1/bin/foo
      lib/
        libfoo.so -> ../../packages/foo/1.1/lib/libfoo.so
  latest -> profiles/default
  packages.db                   # binary DB (hashmap)
  tmp/                          # temporary workdir for atomic ops
  keys/                         # trusted public keys
  .lock                         # flock lockfile
```

---

## 4 — Manifest format

Keep it minimal and extensible. Plain text, one entry per line.

**Line format (minimal):**

```
<relative-path>
```

**Extended line format (optional columns):**

```
<relative-path> <octal-mode> <sha256>
```

* `relative-path`: path relative to package root (e.g. `bin/foo`, `lib/liba.so`)
* `octal-mode`: `0755` style field (optional; used for verification; symlinks are preserved)
* `sha256`: hex-checksum of file contents (optional; used for integrity checking)

Put `.manifest` at package root:

```
~/.pandora/packages/<name>/<version>/.manifest
```

`PKGINFO` (text in package dir) should include `name`, `version`, `author`, `keyid`, `archive_checksum`, and `manifest_checksum` for auditing/repair.

---

## 5 — DB on-disk format (precise spec)

**File:** `~/.pandora/packages.db`
**Endianness:** little-endian (document in header)
**Integer sizes:** fixed-width types (`uint32_t`, `uint8_t`); switch to 64-bit offsets if you expect >4GiB pools.

### Header (fixed-size)

* `char magic[8]` = ASCII `"PANDORA\1"` (zero terminated or version byte)
* `uint32_t version` — DB format version (start at `1`)
* `uint32_t pkg_count` — number of `PackageDisk` entries
* `uint32_t bucket_count` — number of buckets (power-of-two)
* `uint32_t pool_size` — bytes length of string pool
* `uint32_t reserved[3]` — zero (future use)

### Buckets array (`bucket_count` entries)

Each bucket (8 bytes):

* `uint32_t hash` — 32-bit hash (fnv1a suggested) of package name
* `uint32_t head_index` — index into PackageDisk array (0..pkg\_count-1) or `0xFFFFFFFF` for empty

(Using `head_index` → linked list avoids collisions overwriting.)

### PackageDisk array (`pkg_count` entries)

Each entry (packed, align 4):

* `uint32_t name_offset` — offset into string pool
* `uint32_t version_offset`
* `uint32_t author_offset`
* `uint32_t sig_keyid_offset` — key id of signer, offset into pool
* `uint32_t manifest_offset` — offset pointing to manifest path or small inline info (optional)
* `uint32_t dep_count` — number of dependency pairs
* `uint32_t dep_offset` — offset to `dep_count` pairs stored as `name\0version\0...`
* `uint32_t next_index` — index of next package in chain for this bucket (`0xFFFFFFFF` = null)
* `uint8_t active` — 0 or 1
* `uint8_t padding[3]`

### String pool

`pool_size` bytes of packed null-terminated strings:

* package names, versions, authors, keyids, manifest path, dependency name/version strings, etc.

### Sanity checks on load

* `magic` & `version` correct
* `bucket_count` power-of-two and reasonable
* offsets < `pool_size`
* bucket chains only reference valid `pkg_count` indexes

If checks fail, do not use DB; fall back to rebuild from `packages/` or `packages.db.bak`.

---

## 6 — DB operations (semantics & algorithms)

### Insert (install)

* Load DB into memory or create new DB (e.g. `bucket_count = 8`).
* Append strings for name/version/author/keyid/manifest/deps to pool.
* Append `PackageDisk` entry:

  * `next_index = buckets[idx].head_index`
  * `buckets[idx].head_index = new_index`
  * set `active = 0`
* Atomically write DB: write to `packages.db.tmp`, `fsync(fd)`, close, optionally `fsync` parent dir, then `rename()`.

### Lookup by name/version

* `h = fnv1a(name)`, `idx = h & (bucket_count - 1)`.
* Iterate chain: start at `buckets[idx].head_index`, follow `next_index`. Compare names and versions via pool offsets.

### Delete (remove a version)

* Load DB, find entry and unlink from chain by fixing previous `next_index` or `head_index`.
* Option: mark tombstone (e.g. `name_offset = 0xFFFFFFFF`) to avoid reindexing; run compaction later.
* Write DB atomically.

### Grow buckets (rehash)

* Trigger when `pkg_count / bucket_count > 0.7`.
* Allocate new bucket array of double size in memory.
* Iterate non-deleted packages and re-insert into new buckets by recomputing hash & setting `next_index`.
* Replace `buckets` and write DB atomically.

---

## 7 — Activation / forest rebuild / rollback (step-by-step)

### Profiles

* Keep per-profile directories: `~/.pandora/profiles/<profile>/...`
* `latest` is a symlink to a profile (`~/.pandora/latest -> profiles/default`).
* Activation selects a concrete set of `(name,version)` pairs for a profile.

### Rebuild forest — exact steps

1. **Lock** (flock) the global lock file.
2. Create temp directory: `profiles/_new.<pid>` (use `mkdtemp` style).
3. For each `(name,version)` in activation set (resolve deps first):

   * Read `~/.pandora/packages/<name>/<version>/.manifest`.
   * For each `relative-path` in manifest:

     * `mkdir -p profiles/_new.<pid>/<parent>`
     * create symlink: `profiles/_new.<pid>/<relative-path> -> /abs/path/to/.pandora/packages/<name>/<version>/<relative-path>`
     * (Prefer absolute symlinks so profile is independent of CWD.)
4. `fsync()` files/dirs if high durability needed.
5. Atomic swap:

   * If `profiles/default` exists, `rename(profiles/default, profiles/_old.<pid>)`.
   * `rename(profiles/_new.<pid>, profiles/default)`.
   * Optionally keep `_old.<pid>` for rollback; remove it after a retention period.
6. Update DB `active` flags:

   * Mark chosen versions `active = 1`.
   * Mark other versions of same package `active = 0`.
   * Write DB atomically.
7. Unlock.

**Rollback**: keep last `_old.<pid>` and its `.plan` (list of `(name,version)`) to restore by rename and resetting DB flags accordingly.

---

## 8 — Dependency resolution (exact pairs)

* Input: requested root `(name,version)` and its exact (name,version) dependencies.
* Algorithm (DFS/BFS):

  1. Use a stack/queue and `visited` set (size = `pkg_count`).
  2. For each node, read `dep_count` & `dep_offset`. Each dependency is an exact pair `(dep_name, dep_version)`.
  3. If a required dep is missing in DB — fail install/activate (or auto-install if configured).
  4. Detect cycles with 3-state: `0=unseen, 1=visiting, 2=done`. If you encounter `visiting` again → cycle. Default policy: **error** (packager must resolve). Optionally add `--force` to accept cycles.
* On success, produce transitive closure set and, if required, a topological order.

---

## 9 — Signature & verification policy (automatic and required)

* **Algorithm suggested:** ed25519 (small and simple to implement/verify), but pick any deterministic public-key signature algorithm you have libs for.
* **Package contents**:

  * `PKGINFO` (metadata, including `keyid`)
  * `PKG.sig` (signature over `PKGINFO` or canonical bytes of metadata)
  * `.manifest` (list of files; optional per-file checksums)
* **Trust store:** `~/.pandora/keys/<keyid>.pub` and a `trusted_keys` index optionally.
* **Install verification flow:**

  1. Compute archive checksum and/or check `PKGINFO` checksum.
  2. Verify `PKG.sig` over the canonical `PKGINFO` using the public key identified by `keyid`.
  3. If verification **fails**, abort the install; leave partial extraction under `.partial` or delete it.
  4. If verification **passes**, extract to `packages/<name>/<version>.partial`, write `.manifest`, then rename to `packages/<name>/<version>`, then update DB.
* **Runtime checks (optional)**: `rebuild_forest()` or `verify` command can re-check manifest checksums and signatures for installed packages.

---

## 10 — Locking, atomicity, crash recovery

* **Lock**: single `~/.pandora/.lock` file, use `flock()` for operations that mutate DB or profiles (install, activate, remove, rebuild, gc).
* **DB writes**: always write to `packages.db.tmp` → `fsync()` → `rename()` to `packages.db`. Optionally `fsync` parent dir.
* **Forest swap**: use rename with backup (`profiles/default` → `profiles/_old.<pid>`) then `rename(tmp, profiles/default)`; remove backup after success.
* **Partial installs**: extract to `packages/<name>/<version>.partial` and only rename to final name after sign checks succeed.
* **Recovery**:

  * On start, detect `.partial` directories — either resume, verify, or remove.
  * If `packages.db.tmp` exists at boot, inspect and either roll forward or restore `packages.db.bak`.
  * Keep `packages.db.bak` as last known good snapshot before overwrite; optional.

---

## 11 — Housekeeping: GC & compaction

* **GC**:

  * Find versions not referenced by any profile `.plan` and with `active == 0`.
  * Optionally require a safety window (e.g. keep last N backups) before deletion.
  * Remove package directory `packages/<name>/<version>`, then delete DB entry (tombstone or remove).
* **Compaction**:

  * If you use tombstones, periodically rebuild the DB: gather live entries, reassign indexes, rehash buckets into a more compact `bucket_count` (power-of-two).
  * Write compacted DB to `packages.db.tmp` and `rename`.
* **Policy**: run `pandora gc` manually or schedule automatic GC when disk low or tombstone ratio high.

---

## 12 — CLI surface & UX semantics

Minimal command set (semantics):

* `pandora install <pkg-file>`

  * verify signature, extract to `packages/<name>/<version>.partial`, generate `.manifest`, rename to final, write DB.
* `pandora install-remote <repo> <name> <version>`

  * fetch `index` → find URL → download → `install`.
* `pandora activate <name> <version>`

  * resolve deps (exact pairs), build forest (temp + symlinks), atomic swap, update DB `active` flags.
* `pandora deactivate <name>`

  * deactivate package (set active = 0) and rebuild forest or modify profile selection.
* `pandora list [--installed|--active|--all]`

  * list install state and active flag.
* `pandora remove <name> <version>`

  * only allowed when not active and not referenced by any profile; deletes package, updates DB (or tombstones) and optionally triggers compaction on demand.
* `pandora rebuild`

  * force rebuild forest from DB (useful for recovery).
* `pandora rollback`

  * rename last `_old` forest into place and restore DB flags from saved `.plan`.
* `pandora gc`

  * garbage collect unreferenced versions; optional confirm prompt.
* `pandora key add <pubkey-file>` / `pandora key list`

  * manage trusted keys.
* `pandora verify <name> <version>`

  * verify package signature and manifest checksums.

**UX niceties**

* `pandora shell` — spawn a shell with `PATH` and `LD_LIBRARY_PATH` preprended with `~/.pandora/latest/bin` and `.../lib`.
* `profiles`: `pandora profile create/switch/list` for multiple simultaneous sets (per-project envs).

---

## 13 — Example lifecycle (concrete sequence)

### A) Install `foo-1.2.pkg`

1. CLI: `pandora install foo-1.2.pkg`
2. Verify `PKG.sig` over `PKGINFO` with `keyid` using truststore; abort if invalid.
3. Extract to `~/.pandora/packages/foo/1.2.partial/` and write `.manifest` and `PKGINFO`.
4. `rename(...partial, .../1.2)` → atomic finalization.
5. Append entry in `packages.db` (in-memory update + atomic write) with `active = 0`.

### B) Activate `foo-1.2`

1. `pandora activate foo 1.2` → resolve deps (none).
2. Create `profiles/_new.<pid>` and build symlinks using `.manifest`.
3. Atomic swap: move `profiles/default` → `profiles/_old.<pid>` and `_new` → `profiles/default`.
4. Update DB `active` flags (`foo/1.2.active = 1`), write DB atomically.

### C) Install `bar-2.0` which depends on `(foo,1.2)`

1. `pandora install bar-2.0.pkg` → sign verify → extract → write DB (active=0).
2. `pandora activate bar 2.0` → resolver finds `foo-1.2` present → build activation set `{bar-2.0, foo-1.2}` → build forest → swap → update DB flags (bar active; foo remains active).

### D) Rollback (if forest update bad)

1. `pandora rollback` renames `profiles/_old.<pid>` back to `profiles/default` and resets DB `active` flags from stored `.plan`.

---

## 14 — Performance & safety notes

* DB load: reading whole DB into memory is acceptable for thousands of packages; switch to mmap or partial reads if DB grows huge.
* Lookups: hash buckets + chain ensures expected O(1) bucket access; degenerate case handled by rehashing when load factor high.
* Forest rebuild cost: linear in number of symlinks created (cheap). Avoid copying files — use symlinks only.
* Durability: `fsync` file and optionally directory for stronger guarantees; keep `packages.db.bak` before replacement.
* Concurrency: protect DB and profile operations with `flock`.
* Verify/manifests: store checksums in `.manifest` if you want file-level integrity checking.

---
