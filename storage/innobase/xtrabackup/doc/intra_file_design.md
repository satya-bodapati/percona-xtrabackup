# Intra-file parallelism — design

A concise design summary for splitting a single large InnoDB file
across K data-copy workers, with parallel decode at restore. Companion
to [`cloud_direct_design.md`](cloud_direct_design.md) — that doc covers
the multipart cloud-upload story; this doc covers what happens
**inside** a logical file when intra-file is active. A visual
walkthrough exists at `scratchpad/intra_file_walkthrough.html` (HTML
artifact) for whiteboard-style exploration of the same material.

## 1. Why intra-file

Today `--parallel=N` is **inter-file**: N data-copy threads, each
takes a whole `.ibd` from a queue. InnoDB file-size distribution is
heavy-tailed — a few huge tablespaces (ibdata1, occasional multi-TB
user tables) dominate wall clock. At `--parallel=64` one thread owns
the giant file; the rest idle. Restore is even worse: one CPU walks
`decompress → decrypt → pwrite` for the long-pole file, ~30 min for
1 TB on a single core.

Intra-file splits the long-pole file into K page-aligned ranges that
K workers handle independently. Both backup and restore get the
~K× speedup on the long file.

## 2. The central insight: range = file

Don't try to share a `ds_compress` or `ds_encrypt` context across
workers — those have sequential state. Instead:

- The dispatcher splits a large file into K page-aligned ranges.
- Each range gets a **synthetic name** with `.r<idx>` appended at the
  very end (after every other suffix the file would normally carry).
- Each range becomes a separate **work item** in the worker queue.
- Workers consume range work items exactly like file work items —
  they don't need to know they're working on a segment.
- Each range gets its own fresh `ds_compress` + `ds_encrypt` + leaf
  chain. The chain instances are fully independent.
- Output: K self-contained envelopes, one per range. Restore decodes
  them in parallel.

Compress and encrypt sinks are **unchanged**. We instantiate them K
times for split files; the work they were designed to do (read bytes
in order, produce a stream) is unmodified.

## 3. Naming convention — `.r<idx>` always at the very end

The most important rule, future-proof against the existing suffix
vocabulary AND any future suffix additions:

**`.r<idx>` is the RIGHTMOST suffix, applied after every other
file-type suffix.**

Concrete examples covering every file type in the codebase today:

| Logical file | Segment 2 of K=8 |
|--------------|------------------|
| `t1.ibd`                        (full backup)           | `t1.ibd.r2` |
| `t1.ibd.zst`                    (full + compress)       | `t1.ibd.zst.r2` |
| `t1.ibd.zst.xbcrypt`            (full + compress + encrypt) | `t1.ibd.zst.xbcrypt.r2` |
| `t1.ibd.delta`                  (incremental delta)     | `t1.ibd.delta.r2` |
| `t1.ibd.delta.zst.xbcrypt`      (incremental + transforms) | `t1.ibd.delta.zst.xbcrypt.r2` |
| `t1.ibd.new`                    (reduced-lock recopy)   | `t1.ibd.new.r2` |
| `t1.ibd.new.delta`              (reduced-lock + incremental) | `t1.ibd.new.delta.r2` |

Why "at the very end":

- **Composes with existing suffix-stripping**. The apply-side already
  uses `truncate_suffix(EXT_NEW_DELTA, ...)`, `truncate_suffix(EXT_DELTA,
  ...)`, etc. With `.r<idx>` at the very end, the apply-side strips
  `.r<idx>` first (uniformly), then the existing chain of suffix-strips
  runs unchanged.
- **Composes with cloud filename-driven routing**. ds_cloud detects
  compression / encryption suffixes by parsing the filename. With
  `.r<idx>` at the end, ds_cloud strips it first to recover the
  pre-segment name, then applies the same suffix-recognition logic.
- **Composes with hypothetical future suffix additions**. If we ever
  add another file-type suffix (e.g. `.<algo>`, `.<version>`), it
  goes in front of `.r<idx>` and the rule still holds.

The idx is a decimal integer with no leading zeros: `.r0`, `.r1`,
..., `.r15`. Practical max K ≈ 16–32; lexical sort order in
directory listings is not a strict requirement (apply uses the
manifest for ordering, not `readdir()`).

### Files that never get the suffix

These are always emitted unsplit:

- **Tiny control files**: `<space_id>.del`, `<schema>/<space_id>.ren`,
  `<schema>/<table>.ibd.crpt` — empty or a few bytes.
- **Incremental sidecars**: `<name>.meta`, `<name>.new.meta` — a few
  hundred bytes. Never split. Any worker emits them as a regular
  small-file work item; no pinning to a specific range.

### Synthetic-name collision safety

InnoDB filenames must end in `.ibd` (or specific extensions); user
tables cannot legitimately end in `.r<digits>`. So no name collision
with real data files.

## 4. Strip helper — uniform first step on apply

```cpp
// strip_segment_suffix() — invoked FIRST on every name the apply-side sees.
// Returns the pre-segment logical name and the range_idx (-1 if unsplit).
static std::string strip_segment_suffix(std::string name, int &range_idx_out) {
  auto dot_r = name.rfind(".r");
  if (dot_r == std::string::npos || dot_r + 2 >= name.size()) {
    range_idx_out = -1;
    return name;
  }
  auto rest = name.substr(dot_r + 2);
  if (!rest.empty() && std::all_of(rest.begin(), rest.end(), ::isdigit)) {
    range_idx_out = std::stoi(rest);
    return name.substr(0, dot_r);
  }
  range_idx_out = -1;
  return name;
}
```

Every code path that branches on file extension — apply
(`prepare_handle_*_files`), cloud upload (ds_cloud's suffix routing),
xbstream extract (filehash key) — should call this once at the edge
and continue with the logical name + range_idx as parallel pieces of
info.

## 5. xbstream side — what xbstream already gives us

`xbstream -x --fifo-streams=N --fifo-dir=DIR` is **already a
single-process multi-thread coordinator** (xbstream.cc:757–774). N
reader threads share:

- `filehash`: `unordered_map<string, file_entry_t*>` — per-file fd cache
- `ds_ctxt`: shared sink chain
- `mutex`: guards filehash
- per-file `entry->mutex`: fine-grained per-file lock

Two reader threads receiving chunks for the same parent route
through the same filehash entry, take the same mutex, write to the
same fd. Linux `pwrite` at disjoint offsets is kernel-atomic. The
coordination needed for intra-file is **additive** to this existing
structure, not a redesign.

### New chunk types (additive — XBSTCK02 magic bump)

| Type | Position | Purpose |
|------|----------|---------|
| `R` (range header) | once per segment, BEFORE payload | carries `(parent, range_idx, range_count, range_offset, range_size)` |
| `S_REGIONS_END` (`'q'`) | once per segment, AFTER payload, BEFORE PARTIAL_EOF | carries hole `regions[]` for this range — ONLY for compressed/encrypted sparse files; producer only knows the full list once the range read completes |
| `PARTIAL_EOF` (`'p'`) | once per segment, last | self-describing: carries `(parent, range_idx, range_count)`; decouples segment-end from regular `E` chunk so the receiver doesn't rely on R state to interpret it |

`P` / `E` / `S` (existing inline-SPARSE) chunks are unchanged.

### Receiver routing — two maps next to filehash

```cpp
struct parent_state_t {
  int        fd;
  uint32_t   expected_count;
  uint64_t   done_mask;     // 1 bit per range_idx
  std::mutex mu;
};

struct segment_ctx_t {
  std::string parent;
  uint16_t    range_idx;
  uint64_t    range_offset;
  std::vector<hole_t> regions;     // from S_REGIONS_END (compressed sparse only)
};

unordered_map<string, parent_state_t*>  parent_hash;
unordered_map<string, segment_ctx_t>    segment_ctx;
```

On `PARTIAL_EOF` for a segment: apply this segment's regions[]
punch_hole on the parent fd, set the bit in `done_mask`. When
`popcount(done_mask) == expected_count`, close the parent fd — the
parent file becomes externally visible at that point.

**Invariant**: the parent file is closed (visible at the OS file API)
ONLY after every segment has been fully decoded AND its sparse
regions punched. No half-baked intermediate state ever observable.

## 6. The three transforms compose cleanly

| Transform | Composition with intra-file |
|-----------|-----------------------------|
| **Compression** | Per-range fresh `ds_compress` context. Each segment is a self-contained compressed envelope. Cheap to compress dense zeros (zstd compresses long zero runs to near-nothing). |
| **Encryption** | Per-range fresh `ds_encrypt` context with its own IV. Verified in `xbcrypt_write.cc:46` / `xbcrypt_read.cc:56`: every chunk carries its own magic + IV + per-chunk CRC32, no cross-chunk state, no envelope-wide HMAC. Each range is independently decryptable. |
| **Sparse — plain (no transforms)** | Existing inline `CHUNK_TYPE_SPARSE` (`'S'`) flows among `P` chunks for the synthetic segment name. Receiver routes via segment_ctx and applies holes at `range_offset + chunk.offset` in the parent. No new chunk type needed. |
| **Sparse — compressed and/or encrypted** | New `S_REGIONS_END` chunk emitted at END of segment (regions are known only when range read completes). Receiver pwrites decoded dense bytes, then applies punch_hole on `PARTIAL_EOF`. Brief transient over-allocation between last `P` and `PARTIAL_EOF` — bounded by `range_size`, freed within seconds on ext4/xfs/btrfs via extent-level deallocation. |

## 7. ds_segment_writer — the tail sink

A small (~120 LOC) new sink at the tail of the per-segment decode
chain:

```cpp
struct ds_segment_writer_t {
  int       parent_fd;
  uint64_t  range_offset;
  uint64_t  pos;             // running offset within range
};

ssize_t ds_segment_writer_write(ds_segment_writer_t *w, const char *buf,
                                size_t len) {
  ssize_t n = pwrite(w->parent_fd, buf, len, w->range_offset + w->pos);
  if (n > 0) w->pos += n;
  return n;
}
```

The receive-side chain becomes `ds_decrypt → ds_decompress →
ds_segment_writer`. The parent fd never sees encrypted or compressed
bytes — decoding completes before any pwrite to the parent.

## 8. Reduced-lock interaction

Intra-file integration with `--lock-ddl=REDUCED` requires careful
composition with the in-flight PXB-3818 hardening (PR #1763, ~9
commits on the `reduced-lock-fixes` branch).

### What each PXB-3818 fix needs from intra-file

Three groups:

**Compose with explicit segment-aware work:**

- **PXB-3808** (rename swap cycle) — two-pass `prepare_handle_ren_files_to_tmp`
  + `prepare_handle_ren_files` mechanism. Both passes operate on .ibd +
  .delta + .meta. With K segments, both passes need to park / rename K
  segment files per space_id, using a temp name scheme like
  `tmp_<space_id>.<rest>.r<idx>`. The .meta single rename stays unchanged
  (it's never split).
- **PXB-3810** (incremental rename cycle) — same two-pass mechanism's
  source==dest skip-shortcut now only skips the .ibd rename and still
  renames the .delta/.meta. Same K-segment treatment needed for the
  .delta part.
- **PXB-3816** (recopy + rename keeps base alive for incrementals) —
  prepare processes .ren before .new. The .ren-driven rename of the
  base .delta/.meta needs to walk K segments; the subsequent .new.delta
  replacement also needs to walk K segments.

**Promote signal from per-range to per-parent:**

- **PXB-3812** (corruption fail-loud) — `handle_ddl_operations()`
  validates every .crpt is covered by recopy OR drop. The corrupted-
  tablespace signal is **parent-grained** (keyed on `space_id`), so a
  range-worker that hits a bad page calls `add_corrupted_tablespace`
  exactly as today. K-1 sibling ranges either finish their work
  (wasted but bounded) or get a future "abort siblings" optimization.
  At apply time, `prepare_handle_corrupt_files` must delete all K
  segment files via `for_each_segment` (see below). For multipart
  uploads in flight on the affected segments, ds_cloud's existing
  abort-on-error path issues `AbortMultipartUpload`.

**Compose cleanly with no special handling:**

- **PXB-3809** (.del marker after ADD INDEX + DROP) — pure control-file
  logic. The .del marker is tiny and never split. The thing it
  references (the copied .ibd or .new) gets segment-aware cleanup via
  `for_each_segment`.
- **PXB-3811** (IMPORT TABLESPACE detection) — the resulting `.new`
  recopy goes through the normal intra-file path. No new logic.
- **PXB-3813** (cross-DB rename, dest dir creation) — directory
  creation happens once per .ren entry, then K segment renames follow
  into the now-existing directory. No special handling beyond making
  the for-each-segment loop come AFTER the directory creation.

### The for_each_segment glue

```cpp
// Iterate segments of logical_name. If logical_name exists unsplit,
// call op() once with it. Otherwise iterate logical_name.r0, .r1, ...
// until the first miss (segments are contiguous 0..K-1).
static void for_each_segment(const std::string &logical_name,
                             std::function<void(const std::string&)> op) {
  if (os_file_exists(logical_name)) { op(logical_name); return; }
  for (int r = 0; ; ++r) {
    std::string seg = logical_name + ".r" + std::to_string(r);
    if (!os_file_exists(seg)) break;
    op(seg);
  }
}
```

Every existing apply-side function that operates on a `.ibd`,
`.delta`, `.new`, or `.new.delta` file gets refactored to loop
through `for_each_segment` rather than reference the bare name
directly:

- `prepare_handle_corrupt_files` — delete K segment files of the
  corrupted tablespace.
- `prepare_handle_del_files` — delete K segment files of the dropped
  tablespace.
- `prepare_handle_ren_files_to_tmp` + `prepare_handle_ren_files` —
  rename K segment files (twice, once to temp, once to final).
- `prepare_handle_new_files` — the .new → original rename. Note:
  reassembly of K .new segments into a single `.ibd.new` happens
  during xbstream extract (via ds_segment_writer); by the time
  apply's rename pass runs, the K segments are already gone, replaced
  by one `.ibd.new`. So the rename itself stays single-file.

### The producer-side .new path

`copy_for_reduced()` (ddl_tracker.cc:351) loops over `new_tables` and
calls `xtrabackup_copy_datafile_func(node, num, dest_name)` with the
`.new` suffix appended. When `xtrabackup_copy_datafile_func` is
intra-file-aware, large recopies split exactly like full backups:
`ibdata1.new` becomes `ibdata1.new.r0` ... `ibdata1.new.r7`. Each
segment carries its own per-range chain. Receive side reassembles via
ds_segment_writer into a single `ibdata1.new` BEFORE
`prepare_handle_new_files` runs.

The phase ordering (.ren → .new → others) in apply is preserved.
Intra-file changes only the segment count per logical file inside
each phase, not the phase ordering itself.

## 9. Staging recommendation

Risk of regressing the PXB-3818 fixes is real. Stage intra-file
landing:

| Stage | Scope | Surface |
|-------|-------|---------|
| **1** | Full backup, no reduced-lock | dispatcher + R/S_REGIONS_END/PARTIAL_EOF + ds_segment_writer + parent_state. Threshold-gated; only large `.ibd` files. |
| **2** | Incremental backup, no reduced-lock | adds `.r<idx>` to `.delta` synthetic names; per-parent `.meta` as separate queue item; apply-side parallel apply-delta of K segments. |
| **3** | Reduced-lock full | adds `.new` segment handling in `copy_for_reduced`; segment-aware `for_each_segment` rollout in `prepare_handle_corrupt_files`, `prepare_handle_del_files`, `prepare_handle_ren_files`*. |
| **4** | Reduced-lock incremental | adds `.new.delta` and `.new.meta` per-parent coordination; covers PXB-3816-class edge cases (recopy + rename combinations); covers PXB-3808 rename-swap two-pass cycle. |

Each stage gated by a flag (`--intra-file-fanout=N`, default 1 = off)
so production rolls out gradually. Stage 4 needs the dedicated test
matrix from `test/suites/reducedlock/` (rename_swap_cycle,
incremental_rename_cycle, incremental_recopy_rename, etc.) re-run with
intra-file enabled, every DDL combination covered.

## 10. Open questions

- **K sizing default.** Currently sketched as `K = min(--parallel, 8)`
  for GB-scale, `K = 4` for TB-scale. Auto-derive from file size, or
  user-visible `--intra-file-fanout`?
- **Split threshold.** 256 MiB starting value (well above
  `--cloud-multipart-threshold`, captures only long-tail files).
  Confirm against backup distributions from real customers.
- **S_REGIONS_END size pathology.** A 1 TB range with every-other-page
  sparse pattern produces millions of region entries. Typical sparse
  `.ibd` files have 10s to 1000s of runs and fit comfortably. If a
  customer hits the pathological case, the chunk format can grow a
  "streaming regions" continuation marker — leave as a deferred
  extension.
- **Transient over-allocation window** for compressed/encrypted
  sparse. Document operationally; advise users to watch for the brief
  disk-fill spike during the punch_hole window.
- **Sibling-abort optimization** on range corruption. Today K-1
  ranges waste their work when range N hits a bad page. A cross-range
  signal could abort siblings faster. Not load-bearing; revisit if
  customer reports a slow corruption-failure path.
- **Write-filter audit.** Most are page-local and stateless. Changed-
  page-tracking is the non-trivial one — per-range bitmap fragments
  need merging at end-of-backup.
