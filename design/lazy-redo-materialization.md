# Lazy Redo Record Materialization

## Problem

During `--prepare`, every redo record body is eagerly copied from the parse buffer
into a heap-allocated `recv_t::data` chain inside `recv_add_to_hash_table()`, even for
records that will never be applied:

- Pages already flushed to disk (`page_lsn >= rec->end_lsn`) → body copied, then skipped
- Dropped tablespaces (`RECV_DISCARDED`) → entire `rec_list` freed without use
- Multi-batch pressure → when `space->m_heap` fills up, `recv_apply_hashed_log_recs()`
  fires inline mid-scan, frees the heap, and the scan continues forward. Pages applied
  in an early batch may be evicted from the buffer pool before a later batch reaches more
  records for the same page — requiring a re-read from the tablespace file.

The root cause: `recv_add_to_hash_table()` allocates and copies body bytes unconditionally.

```cpp
// log0recv.cc:2774 — current
prev_field = &recv->data;
while (rec_end > body) {          // ← body bytes copied here, always
    recv_data = mem_heap_alloc(space->m_heap, sizeof(*recv_data) + len);
    memcpy(recv_data + 1, body, len);
    ...
}
```

## Solution

Skip the body copy at parse time. At apply time, inside `recv_recover_page_func()`,
fetch the body bytes from `xtrabackup_logfile` on demand — but only after the
`recv->start_lsn >= page_lsn` check confirms the record is actually needed.

Records for pages already up-to-date or in dropped tablespaces are never fetched.

## Data Structure Change

No change to `recv_t`. In the lazy path, `recv->data` is simply left as `nullptr`.
`start_lsn` (already stored) encodes the file position; body bytes are derived at
apply time using `lsn_to_file_offset(start_lsn)` plus the computed header length.

```cpp
// Current recv_t — unchanged
struct recv_t {
    mlog_id_t    type;
    ulint        len;
    recv_data_t *data;      // eager: heap body chain; lazy: nullptr
    lsn_t        start_lsn; // lazy: used to derive file position at apply time
    lsn_t        end_lsn;
    // UT_LIST link
};
```

Per-record heap usage drops from `sizeof(recv_t) + body_len` to `sizeof(recv_t)`.
For N=10M records, B=500 bytes avg: heap drops from ~5 GB to ~320 MB.

## Where the Body Is Read

In `recv_recover_page_func()` (log0recv.cc:3003), the current code sets up `buf`
from `recv->data` **before** the LSN check. The lazy change moves the body fetch to
**inside** the LSN check:

```cpp
// log0recv.cc:3003 — current: buf set up before LSN check
byte *buf = nullptr;
if (recv->len > RECV_DATA_BLOCK_SIZE) {
    buf = ut::malloc(..., recv->len);
    recv_data_copy_to_buf(buf, recv);
} else if (recv->data != nullptr) {
    buf = ((byte *)(recv->data)) + sizeof(recv_data_t);
}
// ...
if (recv->start_lsn >= page_lsn ...) {   // ← LSN check
    recv_parse_or_apply_log_rec_body(..., buf, ...);
}
```

Lazy path — body fetched only when the LSN check passes:

```cpp
// Lazy path: no buf setup before LSN check (recv->data is nullptr)
byte *buf = nullptr;
if (!recv_lazy_fetch) {
    // eager: existing code unchanged
    ...
}

if (recv->start_lsn >= page_lsn ...) {
    if (recv_lazy_fetch && recv->len > 0) {
        // Derive body position from start_lsn + header length
        os_offset_t body_off =
            lsn_to_file_offset(recv->start_lsn)
            + 1   /* type byte */
            + mach_get_compressed_size(recv_addr->space)
            + mach_get_compressed_size(recv_addr->page_no);

        // Read via InnoDB log block API — handles block headers/trailers
        // and redo log encryption transparently
        log_read_record_body(body_off, recv->len, tmp_buf);
        buf = tmp_buf;
    }
    recv_parse_or_apply_log_rec_body(..., buf, buf_end, ...);
}
// ut::free(buf): only reached in eager path for len > RECV_DATA_BLOCK_SIZE
```

`recv->data` has no other consumers — confirmed: only read in this one block.
All other logic in `recv_recover_page_func()` (page reads, mtr, state transitions,
dirty marking) is unchanged.

## Activation Flag

```cpp
// Computed once, before innodb_init(), in xtrabackup_prepare_func()
// innobase_log_file_size: set by xtrabackup_init_temp_log()
// srv_buf_pool_size: set from --use-memory
bool recv_lazy_fetch =
    xtrabackup_lazy_redo_fetch          // CLI flag not disabled by user
    && (innobase_log_file_size / 2 > srv_buf_pool_size);
```

`xtrabackup_lazy_redo_fetch` is a new boolean CLI option (default: `true`).
Users can disable with `--lazy-redo-fetch=OFF`.

When `recv_lazy_fetch = false` (redo fits in memory, k=1): zero code changes in
the existing path. No regression risk.

## Cases Where We Save Work

| Case | Current | Lazy |
|---|---|---|
| Page already up-to-date (`page_lsn >= end_lsn` for all records) | all bodies copied to heap, every record skipped at apply | LSN check fails → zero fetches for this page |
| Dropped tablespace (`RECV_DISCARDED`) | entire `rec_list` bodies freed unused | `recv_recover_page_func()` returns early → zero fetches |
| Intermediate updates (LSN 100, 200, 300; disk has 250) | all 3 bodies stored in heap | only LSN 300 body fetched |
| Large redo, small `--use-memory` | k inline apply phases; pages re-read from tablespace between batches | heap holds only metadata (~56 B/record vs ~556 B/record eager); inline apply threshold ~10× harder to reach; for typical configs k=1 where eager had k>>1 |

## Trade-offs

- **k=1 path (redo fits in memory)**: `recv_lazy_fetch = false`. Existing eager code
  runs unchanged. Zero new code in this path, zero risk.
- **k>1 path (lazy activated)**: body bytes read from `xtrabackup_logfile` on demand
  at apply time (random reads, ~500 B avg). Cost is small on NVMe; for a given page
  its redo records are clustered nearby in the file. The saving is elimination of
  multi-batch tablespace page re-reads, which dominates for large databases on slow storage.
- **Encrypted redo** (`innodb_redo_log_encrypt`): body bytes are read via the InnoDB
  log block API, which handles decryption transparently. Not a blocker.

## Performance Analysis

### Current xtrabackup `--prepare` Flow

```
while (not end of xtrabackup_logfile):
    read next RECV_SCAN_SIZE chunk → recv_scan_log_recs()
    parse records, alloc recv_t + copy body bytes to space->m_heap
    if recv_heap_used() > max_mem:
        recv_apply_hashed_log_recs()   ← inline apply, frees heap
        scan continues from current position (no re-scan)

final recv_apply_hashed_log_recs()
```

Single sequential redo scan. k apply phases interleaved (k = number of times heap
fills). Between phases, pages applied early may be evicted and re-read from tablespace.

### Lazy Flow

```
while (not end of xtrabackup_logfile):
    read next RECV_SCAN_SIZE chunk → recv_scan_log_recs()
    parse records, alloc recv_t, recv->data = nullptr  ← no body copy
    recv_heap_used() never crosses max_mem → inline apply never fires

recv_apply_hashed_log_recs()   ← single apply phase
    for each page: recv_recover_page_func()
        read page from .ibd once
        for each record where start_lsn >= page_lsn:
            read body from xtrabackup_logfile → apply
```

Single sequential redo scan. Heap holds only `recv_t` + `recv_addr_t` metadata (~56 B/record),
so the inline apply threshold is ~10× harder to reach than the eager path. For most practical
configurations k=1; multiple phases are possible but rare (see threshold table below).

### I/O Comparison

| | Redo I/O | Tablespace (page) I/O |
|---|---|---|
| Eager (k batches) | 1 sequential scan | k apply phases; pages re-read if evicted |
| Lazy | 1 sequential scan + on-demand body reads | 1 apply phase; each page loaded once |

### Multiple Apply Phase Threshold

The inline apply fires when `recv_heap_used() > max_mem` (≈ `--use-memory`).

```
Heap per record:
  Eager:  sizeof(recv_t) + avg_body  ≈  56 + 500  =  556 B
  Lazy:   sizeof(recv_t) only        ≈  56 B             (no body bytes in heap)

avg redo record ≈ 510 B  →  ~2M records per 1 GB of redo

Multiple phases when:  N_records × heap_per_rec  >  use_memory
```

| `--use-memory` | Eager: k>1 when redo > | Lazy: k>1 when redo > |
|---|---|---|
| 4 GB | ~4 GB | ~40 GB |
| 2 GB | ~2 GB | ~20 GB |
| 1 GB | ~1 GB | ~10 GB |
| 500 MB | ~500 MB | ~5 GB |

For most production prepare runs (redo 1–20 GB, `--use-memory` 1–4 GB), lazy reduces
k from potentially double-digit to 1. Multiple phases in the lazy path are possible
but require both small memory and a very large redo log.

### When Lazy Wins

```
Eager total I/O  ≈  R/s  +  k · W/r
Lazy total I/O   ≈  R/s  +  f·N·B/r_log  +  W/r

Lazy wins when:  (k-1) · W/r  >  f·N·B/r_log
```

Where `r_log` is random read bandwidth on the logfile device (typically fast local NVMe),
`r` is random page I/O bandwidth, `W` = tablespace working set, `f` = fraction of records
that need apply. For typical workloads with partial redo application (`f < 1`) and slow
tablespace I/O (`r << r_log`), lazy wins decisively for any `k ≥ 2`.

## Implementation

### Files Modified

| File | Change |
|---|---|
| `storage/innobase/include/log0recv.h` | `extern bool recv_lazy_fetch` (under `#ifdef XTRABACKUP`) |
| `storage/innobase/log/log0recv.cc` | `bool recv_lazy_fetch = false;` global; `recv_lazy_read_body()` helper; gate body copy in `recv_add_to_hash_table()`; gate buf setup + add lazy fetch in `recv_recover_page_func()` |
| `storage/innobase/xtrabackup/src/xtrabackup.cc` | `bool xtrabackup_lazy_redo_fetch = true;`; `OPT_XTRA_LAZY_REDO_FETCH`; `--lazy-redo-fetch` option; `recv_lazy_fetch` computation before `innodb_init()` |

### Body Fetch: LSN Semantics

InnoDB LSN includes every raw byte in the log file (including block headers/trailers).
`recv_calc_lsn_on_data_add(lsn, data_len)` accounts for block overhead when advancing
the LSN by `data_len` stripped data bytes. Therefore:

```
body_start_lsn = recv_calc_lsn_on_data_add(recv->start_lsn, hdr_len)
hdr_len = 1 + mach_get_compressed_size(space_id) + mach_get_compressed_size(page_no)
body_end_lsn = recv->end_lsn
raw_bytes_to_read = body_end_lsn - body_start_lsn  (includes block overhead)
```

`recv_read_log_seg(*log_sys, raw_buf, body_start_lsn, body_end_lsn)` reads the raw
bytes. The helper `recv_lazy_read_body()` then strips block headers (12 B) and trailers
(4 B) every 512 B to produce exactly `recv->len` data bytes. InnoDB log block API
handles redo log encryption transparently — encrypted redo is not a blocker.

### Redo Block Layout and `m_first_rec_group`

Each 512-byte log block:

```
[bytes 0..11]   = LOG_BLOCK_HDR_SIZE (12 B framing)
                   ├─ block_no (4)
                   ├─ data_len (2)
                   ├─ m_first_rec_group (2)   ← MTR-start offset (or 0)
                   └─ checkpoint_no (4)
[bytes 12..507] = data region (496 B of record stream)
[bytes 508..511] = LOG_BLOCK_TRL_SIZE (4 B checksum)
```

`m_first_rec_group` is the offset of the first fresh-MTR start within the block,
or 0 if the entire block is continuation bytes of an MTR that started earlier.
It exists so a reader starting mid-log (crash recovery, parallel-parse thread
boundaries) can find a safe MTR boundary to begin parsing.

**Why lazy fetch ignores `m_first_rec_group`:** we are not picking a parse start
point. The exact LSN of each record's body was recorded in `recv->start_lsn`
during the initial single-threaded scan phase. At apply time we `pread` that
exact LSN range back and strip block framing. The 12-byte block header —
including `m_first_rec_group` — is unconditionally skipped as noise.

**Continuation blocks and multi-block MTRs:** a large MTR (e.g. one touching
many pages back-to-back before `MLOG_MULTI_REC_END`) writes records across many
blocks; most of those blocks have `m_first_rec_group == 0`. A single record body
may also span several blocks. Both cases require no special handling — the
stripping loop copies the 496 data bytes of each block and skips 16 framing bytes
at each boundary, regardless of whether the block carries a fresh MTR start.

MTR boundary markers (`MLOG_MULTI_REC_END`, the multi-rec flag on the first
record of an MTR) were consumed by the parse phase. They are irrelevant to
fetching one record's body at apply time.

### CLI Flag

```
--lazy-redo-fetch[=ON|OFF]
    Enable lazy redo record body fetching during --prepare.
    When ON and redo log size > --use-memory / 2, record bodies are fetched
    on demand at apply time instead of being copied to the heap during parse.
    This eliminates multi-batch tablespace page re-reads.
    Default: ON (auto-detected based on redo size vs --use-memory).
    Use --lazy-redo-fetch=OFF to force the original behaviour.
```

## Benchmark Plan

### Case 1 — Redo fits in memory (`recv_lazy_fetch = false`)

```bash
xtrabackup --prepare --use-memory=16G --target-dir=/backup
```

**Goal**: zero regression vs current code. Lazy path not activated; existing eager
code runs unchanged.

### Case 2 — Redo does not fit (`recv_lazy_fetch = true`)

```bash
xtrabackup --prepare --use-memory=1G --target-dir=/backup
```

**Metrics**: wall time, peak RSS, page re-read count, redo bytes read.
**Expected**: lower wall time, lower peak RSS, zero page re-reads.

**Sweep** across `--use-memory` values (16G → 512M) to find break-even point.
