# xbstream format

This document describes the xbstream wire format including the
9.7.1-rc1 extensions: the new `XB_STREAM_FLAG_SINGLE_OBJECT` chunk
flag and the now-authoritative offset semantics. For a refresher
on the overall manifest design, see `manifest_format.md`.

## Chunk header

Every xbstream chunk has a fixed-prefix header followed by the
chunk-type-specific tail:

```
+----------------------+
| magic (8 bytes)      |  "XBSTCK01"
+----------------------+
| flags (1 byte)       |  bit field, see below
+----------------------+
| type  (1 byte)       |  'P' = PAYLOAD, 'S' = SPARSE, 'E' = EOF
+----------------------+
| pathlen (4 bytes LE) |
+----------------------+
| path (pathlen bytes) |
+----------------------+
| ... type-specific tail
```

`PAYLOAD` and `SPARSE` chunks carry, after `path`:

```
+----------------------+
| (SPARSE only) sparse_map_size (4 bytes LE)
| length  (8 bytes LE) |  payload byte count
+----------------------+
| offset  (8 bytes LE) |  authoritative write position (see below)
+----------------------+
| checksum (4 bytes LE)|  crc32 of sparse_map + payload
+----------------------+
| (SPARSE only) sparse_map (8 * sparse_map_size bytes)
+----------------------+
| payload (length bytes)
+----------------------+
```

`EOF` chunks omit `length`, `offset`, `checksum`, and `payload`.

## Flags

| Bit  | Name                          | Meaning |
|------|-------------------------------|---------|
| 0x01 | `XB_STREAM_FLAG_IGNORABLE`    | A reader that does not recognise this chunk's type may skip it without erroring. |
| 0x02 | `XB_STREAM_FLAG_SINGLE_OBJECT` | The producer wants this file stored as one complete object on the consumer side rather than split across many. See below. |

Bits 0x04 through 0x80 are reserved.

### `XB_STREAM_FLAG_SINGLE_OBJECT` semantics

When set on a chunk, the producer is signalling that the file the
chunk belongs to was emitted through xtrabackup's
`ds_open_single_object` path — it bypassed any configured
`ds_compress` / `ds_encrypt` transforms, so the payload bytes are
plain. Consumers act:

* **`xbstream -x`** (extract to disk): treat the chunk as a regular
  `PAYLOAD` chunk. The flag does not change extraction routing —
  that has always been driven by the file path's transform
  extension (`.qp.xbcrypt`, `.lz4`, etc.). Single-object paths
  carry no such extension and fall through the existing dispatcher
  to a plain write on disk. The flag is informational here.

* **`xbcloud put`** (upload to cloud): accumulate the chunks of
  any path whose chunks have this flag into a local buffer, then
  upload one PUT to `<backup_name>/<path>` (no chunk-index suffix)
  when the EOF chunk arrives. This is what makes the destination
  cloud object name match the file's original path so operators
  can read it directly without xbcloud or xbstream involvement.

* **`xbcloud get`** (download from cloud): listing the bucket
  partitions objects into chunked (those whose name parses as
  `<path>.NNNNNNNNNNNNNNNNNNNN`) and single-object (bare names).
  Single-object files are downloaded synchronously and re-emitted
  into the output xbstream as `PAYLOAD` chunks with this flag set,
  followed by an `EOF` chunk also carrying the flag.

Old readers that do not understand the flag simply ignore the
unknown bit and treat the chunk normally. The format is
forward-compatible by construction.

## Offset is authoritative

Every `PAYLOAD` and `SPARSE` chunk carries an `offset` field. As of
the 9.7.1-rc1 spec change, this offset is **authoritative**:

> Chunks belonging to one path may arrive in any order, and a
> reader must `pwrite()` the payload at the chunk's offset rather
> than assume sequential delivery.

The wire format did not change — every writer in tree has always
populated the field correctly. The spec note unlocks future
out-of-order readers (e.g. xbstream extract using parallel pwrite,
xbcloud get emitting chunks as parallel range-GETs complete)
without a further format bump.

For now, every writer in tree emits chunks in increasing offset
order per path. Implementations that wish to reorder may do so
freely.

## Self-describing across formats

Combining the chunk-level flag with `xbcloud`'s naming convention
gives a uniform contract across local, xbstream, and cloud
outputs:

| Storage  | Single-object file lands as | Mechanism |
|----------|----------------------------|-----------|
| Local    | Plain file at the path     | `ds_local::open_single_object` |
| xbstream | `PAYLOAD_SINGLE_OBJECT` chunks in stream | `ds_xbstream::open_single_object` |
| Cloud (xbcloud put) | Single object named exactly `<path>` (no `.NNNN` suffix) | xbcloud accumulator + single PUT |
| Cloud (xbcloud get) | Single object → `PAYLOAD_SINGLE_OBJECT` chunks back into output | xbcloud download → `xb_stream_write_*` re-framer |

A backup can be round-tripped through any of these without the
manifest content changing.
