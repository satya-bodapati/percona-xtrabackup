# Parallel redo parse for `--prepare`

Status: working prototype, not shippable. Behind `XB_PARSCAN_THREADS`, off by
default. Branch `PXB-prepare-combined-8.4`.

## Why

Once the apply phase stops waiting on I/O, the redo *scan* is the larger half
of prepare. Measured on a 21.3 GB redo corpus (1.09 B records, 261,038
distinct pages) at `--use-memory=2G`, the realistic 10:1 redo-to-memory ratio:

| phase | before this work | with parallel parse |
|---|---|---|
| scan | 108.1 s | 42.9 s |
| apply | 49.8 s | 50.7 s |
| wall | 158 s | **97-100 s** |

Parse cost is per *record*, not per byte, and the parse itself is pure
computation over a read-only buffer. It is the most parallelisable thing in
prepare.

## The idea

Partition the redo block range into N contiguous chunks, one per worker. Each
worker parses the mini-transactions that **begin** inside its chunk and files
the resulting records into its own `recv_sys_t::Spaces` with its own heap.
After all workers finish, fold those per-worker structures into
`recv_sys->spaces`.

Two properties make this work:

**Ownership is by mtr start.** A worker owns every mtr beginning in its
partition, wherever that mtr ends — it may read arbitrarily far past its own
chunk to finish one. Partitions divide *starts*, not *bytes*. Consequently
execution order is irrelevant: worker 1 may run through partitions 2 and 3
while worker 2 has not been scheduled yet, and the output is unchanged. The
`Image` is read-only and shared; each worker writes only its own state.

**The merge is a list splice, not a copy.** Worker *i* owns a contiguous LSN
range below worker *i+1*'s, so for any one page the per-worker record lists
concatenate *in worker order* into exactly the LSN order the serial parse
would have produced. `UT_LIST` concatenation is four pointer writes and a
count update, so the merge is O(distinct pages per worker), never O(records).
Measured: 766 ms for 8,022,819 splices at 32 workers, against 5,518 ms of
parse.

## Finding where to start: `LOG_BLOCK_FIRST_REC_GROUP`

Every 512-byte log block header carries the offset of the first mtr that
*starts* in that block, or 0 if none does. That is the restart marker: a
worker scans forward from its first block for a non-zero value and begins
there. Validated over 44,673,425 blocks against the serial parse as oracle:
`frg_bad = 0`, `spanned_bad = 0`, `unscanned = 0`.

The exception is worker 0 of a window, which must resume at the *exact* LSN
the previous window stopped at. Using the block marker there would restart at
the first mtr boundary in that block, which can be *earlier*, re-filing
records the previous window already filed.

## Windows

The serial scan reads `RECV_SCAN_SIZE` (64 KiB) at a time, far too little to
divide among workers. The parallel driver reads large windows instead
(`XB_PARSCAN_WINDOW_MB`, default 128) and parses each in parallel.

Window size matters and the curve is monotonic up to a point:

| window | scan_ms | wall | heap_max vs 1.61 GB budget |
|---|---|---|---|
| 8 MB | 109,202 | 186 s | — |
| 16 MB | 81,416 | 155 s | — |
| 32 MB | 58,291 | 134 s | — |
| 64 MB | 48,207 | 104 s | under |
| **128 MB** | **42,869** | **97 s** | under |
| 256 MB | 40,305 | 96 s | **over by 85 MB** |

Small windows destroy the gain: 8 MB means ~2,860 windows, each paying thread
spawn and merge for 512 KB of work per worker. 256 MB buys one second and
exceeds `--use-memory`, because a single window's heap growth outruns the
batch trigger's prediction. **128 MB is the operating point.**

Worker count saturates earlier than window size: 4 workers 115 s, 8 workers
111 s, 16 workers 104 s at a 64 MB window.

## Correctness rules

### Seams

For the workers *that parsed at least one mtr*, in index order:

- `start_{i+1} == stop_i` — worker *i* finishes exactly where *i+1* began
- at most one worker stops short of the window end, and it must be the
  **highest-numbered worker with work** — not necessarily worker N-1

A worker may legitimately have no work in two ways, and both must be skipped
rather than break the chain:

1. its entire partition is interior to one large mtr, so no block has a
   non-zero `FIRST_REC_GROUP` — it finds no start
2. its partition lies past `to_lsn` — it finds a valid start and parses
   nothing

Case 2 is why "has work" must mean **`mtrs > 0`**, not `start_lsn != 0`.
Testing the latter rejected windows whose seams were perfect.

`stop_lsn` is always a valid mtr boundary by construction: `cur` only advances
by a fully parsed mtr, and stays at the last good boundary when a parse fails.
So a seam check can never fail because of an invalid boundary, only because
two workers disagree about which boundary.

### Bounds

- **`to_lsn`**: every worker must stop at it, not only the last. The window can
  extend past the end of recovery, and `to_lsn` is `metadata_last_lsn` for a
  full-backup prepare, not `LSN_MAX`. Without this the workers parse to the
  end of the redo file and apply records from past the end of the backup.
- **pre-checkpoint records**: recovery begins at the start of the block
  containing `checkpoint_lsn`, so the first group of records usually begins
  *before* the checkpoint. The serial parse drops them by counting down a
  **byte budget** per record (`recv_update_bytes_to_ignore_before_checkpoint`),
  which means that when the first mtr is longer than the budget it drops the
  leading records and **keeps the rest**. An LSN floor is not equivalent — it
  drops the whole mtr.

### Per-record fields

Every record of an mtr carries the **mtr's** `end_lsn`, not its own.
`recv_multi_rec()` computes `new_recovered_lsn` once from `total_len` and
passes it for all `n_recs`; only `start_lsn` advances per record. This is not
cosmetic: `recv_recover_page_func()` stamps the page's `newest_modification`
from `end_lsn`, so a per-record value writes a wrong LSN into the page header.

## Memory

Records live in heaps the workers allocated, which `recv_sys->spaces` does not
own. Two consequences:

- `recv_heap_used()` must include them, or the apply batch never fires and the
  buffer pool is exhausted mid-window (`buf_LRU_get_free_block` livelock)
- they must be freed with the hash, from `recv_sys_empty_hash()`

The batch trigger also cannot simply be checked after each window. The serial
path checks every 64 KiB so it cannot overshoot; a window is thousands of
times larger, and a window produces roughly 2.6x its redo bytes in heap —
records average 21 bytes of redo against 55 bytes of `recv_t`, and each worker
keeps its own `recv_addr` per page it touches. The driver therefore measures
heap-per-redo-byte from the previous window and fires the batch early when the
next one would not fit.

A merged `Space` must be given a real heap even though its records were
allocated from a worker's, because `recv_add_to_hash_table()` allocates from
`Space::m_heap` and the serial fallback will do exactly that.

## Side-effecting records

`recv_parse_log_rec()` calls `recv_parse_or_apply_log_rec_body()` for every
record, and workers call it concurrently. That function is **not thread-safe**
for a handful of record types. This is the core compatibility problem, and it
is not merely an ordering question — for two of the types it is a data race.

| record | side effect at parse | handling |
|---|---|---|
| `MLOG_FILE_EXTEND` | file size | `parse_only`; nothing else needed, see below |
| `MLOG_FILE_CREATE` | `fil_system` insert | changes page-record **routing** — single-worker window |
| `MLOG_FILE_DELETE` | `recv_sys->deleted`, `missing_ids`, `fil_system` erase | changes routing — single-worker window |
| `MLOG_FILE_RENAME` | name remap, `space_id` unchanged | does not change routing; order-dependent only per space |
| `MLOG_INDEX_LOAD` | none in prepare | nothing to do — handler is inside `if (!recv_recovery_on)`, backup phase only |
| `MLOG_WRITE_STRING` **on page 0** | `recv_sys->keys` | `parse_only` + LSN-ordered replay |
| `MLOG_TABLE_DYNAMIC_META` | `MetadataRecover` | already per-worker; needs a merge, see PXB-2865 |

### Routing is the dividing line

`log0recv.cc`:

```c
if (space_id == TRX_SYS_SPACE || fil_tablespace_lookup_for_recovery(space_id))
    recv_add_to_hash_table(...);
else
    recv_sys->missing_ids.insert(space_id);
```

CREATE and DELETE change the answer to that test, so they must interleave with
ordinary page records exactly as serial does. Deferred replay cannot fix them:
by the time you replay a DELETE, the workers have already filed page records
for a space that should have been dropped. RENAME and EXTEND do not change
routing and so can be deferred.

They should also be unreachable under the default lock mode. `xb_init()`
takes `LOCK INSTANCE FOR BACKUP` at `xtrabackup.cc:8117`, and `xb_init()` is
called at `:8769` while `xtrabackup_backup_func()` is called at `:8837` — so
with `--lock-ddl=ON` the lock is held before the backup function starts,
before the checkpoint is read and before the redo follower runs, and no DDL
can appear in the copied redo at all. That makes the gate ("refuse parallel
prepare when the backup was taken with `--lock-ddl=OFF`") sound, and
`lock_ddl_type` is recorded in `xtrabackup_info` by `backup_mysql.cc:1883`
so prepare can read the mode rather than guess it. Keep the single-worker
window anyway as a defensive path, so the gate does not have to be airtight
to be safe.

### `MLOG_FILE_EXTEND` needs no replay at all

xtrabackup already runs a dedicated pass **after** recovery
(`xtrabackup.cc`: `innodb_init()` then the `space_extend_thread_func` threads)
that reads `FSP_SIZE` from each tablespace's page 0 — by then fully recovered —
and extends the file to match, in parallel across `xtrabackup_parallel`
threads.

The precedent that settles safety: under `--lock-ddl=reduced`, `parse_only` is
*already* passed unconditionally for all four file ops, so that mode already
never applies an `MLOG_FILE_EXTEND` side effect and relies entirely on this
pass. Making the workers `parse_only` is an existing supported configuration,
not a new risk.

### Encryption

Reached from page-0 `MLOG_WRITE_STRING` on a non-system, non-temp tablespace
during parse (`block == nullptr`). Page-0 `MLOG_WRITE_STRING` is *always*
encryption info — `fil_tablespace_redo_encryption()` sets `found_corrupt_log`
if `len != Encryption::INFO_SIZE`, and other page-0 edits use `MLOG_nBYTES`.

Serial behaviour:

1. parse: skip if the space is loaded and `m_header_page_flush_lsn > lsn`;
   skip if the info is zero-filled (the erase step of an unencrypt — the reset
   happens when the record is *applied*); otherwise stash into
   `recv_sys->keys` by `space_id`, **overwriting unconditionally, with no LSN
   comparison**
2. at tablespace open, `encryption_op_in_progress` comes from the **on-disk**
   page 0, not from redo; then a key is applied only if `m_key_len == 0 ||
   key.lsn > m_header_page_flush_lsn`

So `recv_sys->keys` holds "the last non-zero key seen per space", and the LSN
comparison is against the *page's* flush LSN, not against other candidate
keys. Serial is correct only because records arrive in LSN order.

In parallel this is both a race on a `std::vector` and wrong precedence. It
matters whenever a space has more than one non-zero key record in range:
`ALTER TABLESPACE ENCRYPTION=Y/N` can go Y→N→Y with different master keys and
the same `space_id`, and a master key rotation rewrites every tablespace
header at once. The failure mode is an undecryptable tablespace after restore.

**This is independent of `--lock-ddl` mode.** The copied redo begins at the
server's last checkpoint, which can long predate the backup, so key records
from before the backup lock are in range regardless. And unlike the four file
ops, `fil_tablespace_redo_encryption()` has **no `parse_only` parameter**, so
it is never suppressed for records older than
`backup_redo_log_flushed_lsn`.

**Design:** add `parse_only` to `fil_tablespace_redo_encryption()`, returning
immediately after `ptr += len` (`fil0fil.cc:11239`) — before the flush-LSN
skip and the zero-fill check, so no semantics change and the consumed length
is unaffected. Workers pass `true` and record `(lsn, space_id, INFO_SIZE
bytes)` into a per-worker vector; the driver merge-sorts by LSN and replays
each with `parse_only = false` after the seam check and before the batch
trigger, so keys are in place before any page 0 is applied.

Replaying in exact LSN order reproduces serial behaviour **without depending on
a correct model of the encryption state machine**, which is the argument for it
over adding an LSN guard to the overwrite.

A thread-local "this thread must not produce side effects" flag is the least
invasive way to reach the callers, since `recv_parse_or_apply_log_rec_body()`
is three levels below the worker; the same flag can be OR'd into the
`parse_only` already computed for the four file ops.

### Dynamic metadata

`MetadataRecover::parseMetadataLog()` already ends in
`persister->aggregate()`, which is version-gated and **commutative** — a max
over `(version, autoinc)` with a union of `corrupt_indexes`. So each worker's
instance is already correct in isolation; only the final fold is missing.

That fold is the same primitive PXB-2865 needs to carry metadata across
prepare invocations, so it should land there first as
`MetadataRecover::merge()`. Note that a whole-entry merge must iterate **every**
persister type, not dispatch on one as `parseMetadataLog` does, or
`corrupt_indexes` is dropped while `autoinc` survives.

## Verification

Three layers, because each catches what the others cannot.

**Scan digest** (`XB_SCAN_DIGEST=1`). Every filed record contributes
`crc32(space, page, start_lsn, end_lsn, type, len, body)`, accumulated as
count, sum and sum of squares, from **both** the serial and the parallel
filing paths. Identical triples mean an identical multiset of records.
Summation is commutative, so workers accumulate in any order — that is the
point. Accumulate **per worker** and fold in at merge, not at file time, or
windows that are parsed and then discarded are counted.

**Ordering assertion.** Because the digest ignores order by construction, a
page's records are asserted non-decreasing in `start_lsn` at apply. A
mis-spliced merge would otherwise be silent.

**Datadir diff.** Prepare the same corpus serially and in parallel, then
compare byte for byte, excluding `ibtmp1` — prepare recreates it with 128
fresh rollback segments every run, so it differs between any two runs.

The layers are not redundant. Two examples from this work:

- the parallel path failed to apply the page LSN map filter and filed
  305,574,676 extra records — 39% more heap, 18 extra buffer-pool
  invalidations, 24 seconds. The datadir was **identical**, because the map
  only drops records that are no-ops at apply. Only the digest saw it.
- the `end_lsn` bug produced the right record *count* and a wrong page LSN
  stamp. `recs_applied` differed in its last digit, the ordering assertion
  passed, and the datadir diff missed it entirely at a 64 MB window.

Neither the digest nor the diff can see side effects on `recv_sys` state.
`recv_sys->keys` and the recovered metadata map need their own serial-versus-
parallel comparison.

## Failure handling

A window that cannot be handled in parallel is discarded whole — nothing
merged, no side effects — and retried. The driver must hand back **the LSN
filing actually reached**, block-aligned, on every exit path; returning a
window boundary sends the serial parse back over ground already covered.

Note that "falls back, so it costs time and never correctness" is only true of
the failed window. Earlier windows leave records and spaces behind and the
fallback resumes on top of that state.

The serial fallback currently loses one mtr on resume — six records on the
benchmark corpus. With the seam classification fixed the fallback no longer
runs, so this is latent rather than active, but it should be fixed on its own
merits rather than relied upon.

## What is not done

- `MetadataRecover` merge (blocked on PXB-2865)
- `recv_sys->keys`: `parse_only` parameter, per-worker collection, ordered replay
- single-worker window for CREATE/DELETE, and the `--lock-ddl` gate
- the latent serial-fallback off-by-one-mtr
- **tests**: the only corpus used is sysbench inserts — no DDL, no encryption,
  full backup only. Every test passes with all of the above broken. Needed:
  a synthetic-redo unit test for the partitioning corner cases (an mtr larger
  than the worker buffer, an mtr larger than a window, a partition entirely
  interior to one mtr, partitions past `to_lsn`), and corpora with a master
  key rotation before a `lock-ddl=ON` backup and with
  `ALTER TABLESPACE ENCRYPTION` under `reduced`.
- two escalation paths with no implementation: an mtr larger than the worker's
  4 MB stitch buffer (serial grows `recv_sys->buf` up to
  `srv_log_buffer_size`; workers just give up), and an mtr larger than a whole
  window, where `done_lsn` never advances and the driver makes no progress
