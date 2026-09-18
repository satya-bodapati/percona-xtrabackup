# Page LSN map

Avoiding random I/O during `--prepare` for pages that do not need redo apply.

Percona XtraBackup 8.4. Measurements in this document were taken on a single
host: 128 cores, 498 GB RAM, two NVMe devices, Percona Server 8.4.8,
RelWithDebInfo build.

---

## 1. Problem

`--prepare` replays the redo copied during the backup. For each redo record it
must decide whether the change is already present in the page. That decision
needs the page's `FIL_PAGE_LSN`, and reading that field means reading the page.

So every page named anywhere in the redo is read, and a large share of those
reads find a page that is already up to date. Nothing is applied and the read
was pure cost.

It compounds when the redo does not fit in `--use-memory`. The recovery hash
stores records until it hits that budget, then applies what it has, frees the
heap and resumes scanning. Each of those apply batches ends in
`buf_pool_invalidate()`, so a page named in several batches is read once per
batch.

Measured on 45 GB across 64 tablespaces with about 5 GB of redo, at
`--use-memory=512M`: **6,263,521 page reads across 40 apply batches, of which
3,077,424 (49%) found nothing to apply.**

## 2. Approach

When the backup's copy thread reads a page, that page is already in memory.
Reading its `FIL_PAGE_LSN` there costs nothing. Record it, ship it with the
backup, and prepare can make the same decision without the read.

Nothing writes to the backup copy after the copy thread produces it, so the
recorded LSN is authoritative for the file prepare will operate on.

> Drop the record when `record_lsn < copy_lsn(page)`.

A page whose records are all dropped never enters the recovery hash, so
`recv_read_in_area()` never submits it and the read never happens.

The whole feature is one comparison moved earlier in time.

## 3. Backup side

### 3.1 Where the capture hooks

In the copy loop of `xtrabackup_copy_datafile_func()`, immediately after
`xb_fil_cur_read()` returns `XB_FIL_CUR_SUCCESS`, iterating
`cursor->buf_page_no .. + cursor->buf_npages`.

This is the only point that satisfies all three requirements at once:

- **Every page, including on a full backup.** It sits above the write filter.
  `wf_incremental_process()` is not usable: it only runs for `--incremental`,
  and on a full backup `wf_wt_process()` never iterates pages at all.
- **After the checksum retry loop.** `xb_fil_cur_read_from_offset()` re-reads
  the whole batch on a corrupt page; SUCCESS is returned only once that loop has
  converged.
- **The bytes that land in the backup.** `wf_wt_process()` writes exactly
  `cursor->buf` for `buf_npages` pages.

`FIL_PAGE_LSN` is at offset 16, inside the 38 header bytes that
`Encryption::encrypt_low()` copies in the clear, so encrypted and
page-compressed tablespaces are read directly with no decryption.

### 3.2 Temp files

One append-only file per copy thread, created under `--tmpdir`, never fsynced,
unlinked when the backup ends. One file per thread means no locking.

### 3.3 Emission

At the end of `backup_finish()`, after `unlock_all()` and before
`report_backup_size()`. Everything the emit step needs is final by then: every
copy thread has joined, `handle_ddl_operations()` has run, and the redo thread
has stopped. It writes through `ds_data`, so the file rides xbstream,
`--compress` and `--encrypt` exactly like `xtrabackup_info`.

The shipped file is named `xtrabackup_page_lsn`.

### 3.4 Reduced-lock DDL

`--lock-ddl=reduced` is a first-class mode — it is the default for PXC SST — so
the map has to work under it. The mechanism below is designed; it is **not yet
implemented**, and the current build skips the map with a warning when reduced
lock is in use rather than producing one that could be wrong.

The problem it has to solve: under reduced lock a tablespace can be re-copied
after DDL, so both the abandoned run and the `.new` run appear in the temp
files. The map must keep the entries from the run that won and discard the
other. Under `--lock-ddl=ON` none of this runs: no space is re-copied, every
block is generation 0, and the resolved state is empty.

**Generation is binary, not a counter.** Generation 0 is the main copy phase;
generation 1 is the re-copy issued by `copy_for_reduced()` inside
`handle_ddl_operations()`. No space reaches generation 2 — that function runs
once, iterates `new_tables` once, and every `add_*` path on the tracker asserts
`!handle_ddl_ops` from that point on.

**The two generations can never be written concurrently.**
`xtrabackup_backup_func()` blocks in its join loop until every data copy thread
has exited, and `handle_ddl_operations()` opens by calling
`fil_close_all_files()`, `fil_close()` and `fil_init()` — a surviving copy thread
would be reading through fil nodes that no longer exist. Each is independently
sufficient. A block's generation is therefore fixed by the phase that wrote it,
and no timestamp comparison or cross-file ordering is ever needed.

**Resolving which generation won.** The generation byte says which generation a
block belongs to, not which one won. Two independent sources answer that before
the filter opens a single temp file:

- *The tracker.* When `handle_ddl_operations()` returns, `new_tables` is exactly
  the set of spaces carrying a generation-1 blockset, and
  `drops ∪ corrupted_tablespaces ∪ deleted_undo_files` is the tombstone set.
  Both are in memory, both are final, and both are already built for reasons
  unrelated to this feature.
- *A per-file trailer.* Each copy thread appends one record per space it wrote —
  `(space_id, max_generation, first_block_offset)` — then a count, a CRC32C and a
  magic. The filter reads N trailers, where N is the number of temp files rather
  than the number of tablespaces, so it stays in the kilobytes even on an
  instance with a million small `.ibd` files.

The trailer is not redundant with the tracker: it makes a temp file mean
something on its own instead of deriving its meaning from the filename it happens
to carry. The two are derived independently, so agreement is real evidence.

**Precedence**, in this order, because `handle_ddl_operations()` promotes
corrupted and recopy spaces into `new_tables` and erases dropped ones from it:

| condition | decision |
|---|---|
| space in `new_tables`, block generation 1 | keep |
| space in `new_tables`, block generation 0 | skip by `byte_length` |
| space in the tombstone set | drop, whatever the generation |
| in neither set | keep |

Three of the four outcomes never touch the block body, so the filter's cost
tracks redo volume rather than instance size.

**Keep-last is safe here** because a `.new` copy completes before the backup
does — a backup that finishes is proof its re-copies finished. Discarding by
generation also removes the abandoned run wholesale, which matters because pages
read out of a file being rebuilt underneath can carry a torn `FIL_PAGE_LSN`, the
only path to an overstated LSN.

**Duplicates within a generation** come from checksum retries, which append the
page again out of order. Keep the first: the earlier read has the lower LSN, so
prepare applies records already present and the check inside apply discards them.
One comparison, no buffering.

Four details that the 8.4 code requires and that an implementation must handle:

- `deleted_undo_files` is a **local variable** in `handle_ddl_operations()`, not a
  tracker member. It has to be published for the filter to see it.
- `new_tables` is mutated repeatedly inside `handle_ddl_operations()` — erased for
  spaces already copied without lock, added to from `recopy_tables`, erased for
  drops, added to from renames, filtered by `check_if_skip_table`, then augmented
  with new undo files. It must be read **after** that function returns, never
  cached earlier.
- Generation 1 writes a **`.new` sibling**, not an overwrite, so it is legal for a
  space to have generation-0 blocks only, generation-1 blocks only, or both. The
  only genuine disagreement is generation-1 blocks for a space absent from
  `new_tables`; that space should be dropped from the map rather than failing a
  completed backup.
- Renames are irrelevant: `space_id` is stable and page content unchanged, and
  prepare's `.ren` handling only moves files. Keying on `space_id` is correct.

## 4. File format

A block covers a contiguous run of pages of one tablespace. Page numbers are
therefore implied by position and cost nothing to store. The same format is used
for the per-thread temp files and for the shipped file, so there is one reader
rather than two.

### 4.1 Block header, 40 bytes

| offset | field | bytes | meaning |
|---|---|---|---|
| 0 | magic | 4 | `XBPL`; used to resync after a bad CRC |
| 4 | format_version | 1 | |
| 5 | flags | 1 | |
| 6 | generation | 2 | copy generation for this space |
| 8 | space_id | 4 | one tablespace per block |
| 12 | first_page | 4 | page number of entry 0 |
| 16 | base_lsn | 8 | the backup's start checkpoint LSN |
| 24 | byte_length | 4 | body size; used to skip or resync |
| 28 | n_entries | 4 | pages covered by this block |
| 32 | crc32c | 4 | over header and body |
| 36 | reserved | 4 | |

Thirty-four bytes of fields, six reserved so a format v2 can add a field without
changing the block size.

### 4.2 Body

A presence bitmap of `n_entries` bits, then one varint per **set** bit, in
bitmap order. Two rules decode it, and neither stores a page number:

- **Position gives the page.** Bit `i` means page `first_page + i`.
- **Order gives the pairing.** The `k`-th value belongs to the `k`-th set bit. A
  clear bit consumes no value bytes.

A clear bit is not missing data. It means the page's LSN is at or below
`base_lsn`, so every redo record in the backup applies to it and the exact value
is never needed. On a full backup most pages sit exactly at that floor and cost
one bit each, which is why the map measures in hundreds of kilobytes.

### 4.3 Why a hand-written varint

Not `mach_u64_write_much_compressed()`. That routine reaches
`mach_write_compressed()`, which dispatches on `xb_log_detected_format` — so the
layout of a file we own would depend on which redo format the source server
happened to use. And 8.4 has no bounds-checked reader for what it produces:
`mach_parse_u64_much_compressed` does not exist, only an unchecked
`mach_read_next_much_compressed_v4` that will run off the end of a truncated
body.

The replacement is LEB128 with an explicit end pointer, about fifteen lines,
which cannot overrun. Header fields still use `mach_write_to_2/4/8` so the format
does not depend on padding or host byte order, and the CRC is `ut_crc32`.

### 4.4 Trailer

Written last: magic, format version, block count, entry count and a CRC over the
block CRCs. Prepare validates the trailer before trusting a single block.

## 5. Prepare side

The map is loaded after the incremental delta merge and the reduced-lock
`.crpt`/`.del`/`.ren`/`.new` resolution, and before `innodb_init(true, true)` —
so the files it describes are the files recovery will touch.

One change in `recv_add_to_hash_table()`, placed at the top of the function
**before** `recv_get_page_map(space_id, true)`, which would otherwise allocate an
empty `Space` as a side effect:

```c
if (page_lsn_map::is_loaded() && type != MLOG_INIT_FILE_PAGE &&
    type != MLOG_INIT_FILE_PAGE2) {
  const lsn_t copy_lsn = page_lsn_map::lookup(space_id, page_no);
  if (copy_lsn != 0 && !xb_redo_record_applies(start_lsn, copy_lsn)) {
    return;
  }
}
```

`recv_read_in_area()`, `buf_page_io_complete()` and `recv_recover_page()` are
untouched. Apply already runs in parallel on the I/O completion threads and does
not need rebuilding.

Lookup is a per-space vector sorted by page number, fronted by a one-entry memo
on `space_id`. Records arrive strongly clustered by space, so the memo removes
the hash lookup for nearly every call, and a null map pointer makes the whole
check one predictable branch.

## 6. Correctness

### 6.1 The errors are asymmetric

| case | consequence | verdict |
|---|---|---|
| entry missing | page is read as today | safe |
| recorded LSN too low | redundant records applied, then discarded by the check inside apply | safe |
| block fails CRC | contributes no entries, its pages read as today | safe |
| **recorded LSN too high** | **a needed record is skipped** | **silent corruption** |

A page left stale this way is internally consistent and passes its own checksum.
No existing validation catches it, and it would surface much later as
unexplained corruption. Every design decision below follows from that asymmetry:
over-include wherever there is any doubt, and drop the whole map rather than
guess.

### 6.2 The three specific hazards

**Doublewrite pages carry other pages' LSNs.** `xb_fil_cur_read_from_offset()`
deliberately skips corruption checking for pages in
`[FSP_EXTENT_SIZE, 3*FSP_EXTENT_SIZE)` of the system tablespace, but still counts
them into `buf_npages`. Those bytes are copies of *other* pages. Recording one
would be an overstated LSN for this page number — the one route to the unsafe
case. They are excluded from capture by page number.

**`MLOG_INIT_FILE_PAGE` must never be dropped.** `recv_recover_page_func()`
reassigns `page_lsn` and zeroes `FIL_PAGE_LSN` when it sees one, and
`recv_page_is_brand_new()` inspects the first record's type on behalf of
`buf_page_io_complete()`, which uses it to decide whether to zero-fill a page
that reads back corrupt. Removing the leading init record changes the meaning of
every later record for that page and can turn a tolerated torn-extend into a hard
corruption report. These records are rare, so the exemption is free.

**The comparison must not drift.** `recv_recover_page_func()` applies a record
when `recv->start_lsn >= page_lsn`. The pre-filter must make the identical test,
so both call one shared predicate, `xb_redo_record_applies()`. The inequality is
not strict: a record whose `start_lsn` equals the page LSN belongs to the mtr
that begins where the page's last modification ended, so its changes are not yet
in the page and it must be applied. Dropping the equality case would discard
records that today's apply path applies.

### 6.3 The test that catches it

Take one backup with `--page-lsn-map`, prepare two copies of it — one consulting
the map, one ignoring it — and compare the prepared datadirs page by page.
Applying the same records in the same order must give byte-identical pages, so
any difference means the map dropped a record recovery would have applied.

This is the reason the option is a single flag used in both phases rather than a
backup-side flag alone: a map-bearing backup must be preparable both ways from
one binary.

Result: **2,886,087 pages across 67 files, byte-identical.** A small
`--use-memory` is used so recovery runs several apply batches, which is where the
map does most of its work.

## 7. Failure and fallback

The fallback is current behaviour, at every granularity:

- map file absent, wrong version, or bad trailer: ignored entirely
- one block fails its CRC: that block contributes no entries
- one page has no entry: that page is read as today

No failure mode aborts the prepare, and none produces a wrong result. That
property is what would make this safe to enable by default later.

## 8. Interface

One option, `--page-lsn-map`, off by default, passed to both commands:

| command | effect |
|---|---|
| `--backup --page-lsn-map` | writes `xtrabackup_page_lsn` into the backup |
| `--prepare --page-lsn-map` | consults it; falls back silently if absent or unusable |
| `--prepare` | ignores any map present |

Omitting it at prepare is the operator escape hatch and the mechanism the
differential test above needs.

## 9. Sizing

Measured over eight backups of the same 45 GB dataset:

| backup | map | page entries | bytes/page |
|---|---|---|---|
| no flushing (control) | 323 KB | 2,528,967 | 0.128 |
| insert | 404 KB | 2,978,119 | 0.136 |
| 1 space, uniform | 387 KB | 2,541,255 | 0.152 |
| 64 spaces, skewed | 744 KB | 2,541,255 | 0.293 |
| 64 spaces, uniform | 919 KB | 2,530,631 | 0.363 |
| delete | 2.13 MB | 3,168,583 | 0.672 |
| wide rows | 2.78 MB | 3,635,271 | 0.765 |
| update + secondary index | 2.54 MB | 3,276,096 | 0.776 |

0.128 bytes per page is exactly one bit — the floor. Everything above it is the
varint for pages modified during the backup window, whose width is set by **redo
volume**, not by instance size.

Projected to a 1 TB backup:

| case | 16 KB pages | 4 KB pages | share of backup |
|---|---|---|---|
| floor | 8 MiB | 32 MiB | 0.0008% |
| typical update workload | 24 MB | 97 MB | 0.002% |
| heaviest measured | 52 MB | 208 MB | 0.005% |
| worst case | 328 MiB | 1.28 GiB | 0.12% |

The worst case requires every page in the instance to be modified during the
backup window, implying about a terabyte of redo. It is a ceiling, not a
forecast, but it is why a metadata budget belongs in the design (section 12).

## 10. Results

45 GB across 64 tablespaces. The same captured backup is replayed for both cells
of each pair, so the redo bytes are identical by construction and the only
difference is whether prepare consulted the map.

| workload | use-memory | page reads | fewer | batches | wall |
|---|---|---|---|---|---|
| update, 64 spaces | 8G | 805,584 → 369,867 | 54% | 3 → 2 | 73.7 → 53.8 s |
| update, 64 spaces | 512M | 6,263,521 → 3,099,918 | 51% | 40 → 29 | 141.4 → 98.9 s |
| update, 64 spaces | 256M | 7,695,010 → 4,063,236 | 47% | 80 → 57 | 182.3 → 132.3 s |
| update, 64 spaces | 128M | 8,649,543 → 4,750,709 | 45% | 163 → 116 | 240.2 → 178.3 s |
| update, skewed | 512M | 3,255,632 → 1,607,114 | 51% | 41 → 22 | 127.0 → 77.2 s |
| update + secondary index | 2G | 6,454,768 → 3,143,937 | 51% | 11 → 9 | 177.0 → 135.3 s |
| delete | 2G | 3,800,635 → 1,509,070 | 60% | 8 → 5 | 101.9 → 82.0 s |
| wide rows, VARBINARY(4000) | 512M | 2,330,611 → 823,285 | 65% | 64 → 16 | 77.0 → 36.0 s |
| 1 space, uniform | 2G | 56,620 → 19,922 | 65% | 9 → 2 | 34.9 → 23.5 s |
| **insert** | 2G | 132,733 → 126,626 | **4.6%** | 4 → 3 | 28.6 → 27.1 s |

Reads that found nothing to apply fell by about 90% in every case.

### 10.1 Two things that bound the benefit

**Insert-heavy workloads gain almost nothing.** A freshly allocated page has no
earlier version to skip against, so there is almost nothing to drop. The benefit
tracks what the workload did, not how large it is.

**The source server must be flushing during the backup window.** A page only
becomes skippable once its changes have reached the file being copied. A control
captured with `innodb_adaptive_flushing=OFF` and `innodb_io_capacity=100` against
a 64 GB buffer pool measured `pages_wasted = 0` and exactly zero benefit — page
reads identical to the digit. This is worth stating because it inverts the
intuition: the payoff is not set by redo volume or instance size.

### 10.2 Confidence

Counter-based figures (`pages_read`, `batches`, `pages_wasted`) are exact
integers emitted by the binary. Wall-clock figures are single runs, and a control
pair doing provably identical work differed by 19.6%, so treat the clock as
directional. The direction was consistent across every pair.

## 11. Cases not covered yet

In both cases below the option is **accepted and ignored with a warning**, never
rejected. Operators pass one option set to every xtrabackup invocation in a
script or wrapper; failing a backup because it happens to be incremental would
make the feature awkward to adopt and buys no safety. The map is simply not
produced, or not consulted, and the run behaves exactly as it does today.

- **`--incremental`.** Low value rather than a hard barrier, and the value
  argument is the decisive one. An incremental backup copies only changed pages,
  and its redo covers a shorter window, so there is less redo to hold, fewer
  apply batches to collapse, and therefore less of the repeated page reading this
  feature exists to remove. A smaller share of incremental prepare time sits in
  redo apply at all: most of it is the delta merge in
  `xtrabackup_apply_deltas()`, which this feature does not touch. The
  `delta_merge_ms` counter added with this work would size that directly, and it
  should be measured before any effort goes here.

  There is a secondary question that would also need settling. Capture sits above
  the write filter, so it records pages that `wf_incremental_process()` then
  skips for having an LSN at or below `incremental_lsn`; after the merge those
  pages come from the base backup rather than from this delta. That is *probably*
  safe — a page unmodified since the base has the same LSN in both, and a prepared
  base only raises it, which errs low and therefore errs safe — but it is
  unverified reasoning, not a tested property, and unverified is not a basis for
  shipping. If incremental is ever pursued, the clean answer is to move capture
  inside `wf_incremental_process()` after its `continue` guards, so an entry
  exists only for pages actually written into the `.delta`.

- **`--lock-ddl=reduced`.** Designed in section 3.4, not yet implemented. The
  block header already carries the `generation` field it needs; the remaining
  work is resolving generations against the ddl_tracker and the per-file
  trailers. Because reduced lock is the default for PXC SST this is the first
  follow-up, not an exclusion — the feature is incomplete until it lands.

## 12. Considered and rejected

**Filtering the map by a redo-touched page bitmap.** The original proposal was to
drop entries for pages redo never names, on the assumption it would shrink the
map. Measurement says the opposite. The current format gets page numbers free
from dense contiguous runs; filtering makes those runs sparse, forcing page
numbers to be encoded explicitly at roughly 2 bytes each. On the 64-tablespace
workload that turns a 919 KB map into an estimated 1 MB one. Filtering only wins
for a very large instance with very little redo — where the map is already
negligible in absolute terms.

**Skipping `buf_pool_invalidate()` between apply batches.** Every
memory-triggered batch discards the entire buffer pool, which looked like the
largest cheap win available. Built behind a flag and measured: suppressing it on
39 of 40 batches changed page reads from 6,263,521 to 6,263,507 — fourteen reads
in six million. At 512M the pool holds 32,765 frames while each batch touches
about 156,000 pages, so LRU has evicted everything before the next batch begins.
The invalidation is free because the pages were leaving anyway. The code was
removed; making the skip genuinely safe would have required proving that pages
left resident with unmerged change-buffer entries cannot be flushed in a bad
state, and the measurement says not to start.

## 13. Known gap

**Metadata budget.** The design called for abandoning the map once it grows past
a configured cap, on the principle that a backup failing because its optimisation
metadata grew too large would be worse than no optimisation. Not implemented. The
~1.3 GiB worst case in section 9 is the reason it belongs before GA.

## 14. Rollout

Ship behind the flag, off by default, so the tested path remains the fallback.
Promotion to on-by-default is a separate decision, and the fallback behaviour in
section 7 is what makes that decision safe to take later.

The differential test in section 6.3 is the gate. It is the only test that
catches a too-high recorded LSN, so it is not optional, and it should run across
workload shapes: bulk load, wide rows, heavy updates, many small tablespaces, and
compressed and encrypted tablespaces.

## 15. References

- Measured results, with charts:
  https://claude.ai/artifact/Uws99bqBxwS1seBjdc8SA5
- `PXB-3786` — fetch-on-apply (storing redo descriptors instead of bodies).
  Evaluated alongside this work against the same problem. It attacks the batch
  count from the other end and was dropped: on small-record OLTP redo it is 1.44x
  to 1.85x slower, and on the large-record redo where it does win it adds only
  15% on top of this feature.
- Implementation: branch `PXB-page-lsn-map-8.4`.
  - `storage/innobase/xtrabackup/src/page_lsn_map.{h,cc}` — format, writer, reader
  - `storage/innobase/xtrabackup/src/xtrabackup.cc` — capture hook, option, load
  - `storage/innobase/xtrabackup/src/backup_copy.cc` — emission
  - `storage/innobase/log/log0recv.cc` — the drop, the shared predicate, counters
  - `storage/innobase/xtrabackup/test/suites/page_lsn_map/` — differential and
    fallback tests
