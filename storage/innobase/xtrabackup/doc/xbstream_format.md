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
the 9.7.1-rc1 spec change, the offset is **authoritative for write
placement**:

> Producers emit `PAYLOAD` / `SPARSE` chunks for a given path in
> offset-ascending order, with `EOF` last. Receivers may use the
> offset field to `pwrite()` chunks to the destination file in any
> order convenient to them — for example, a parallel worker pool
> can dispatch chunks across threads as they're read, each calling
> `pwrite()` at the chunk's offset.

What changes with this spec note: receivers no longer have to
`write()` chunks sequentially. They can buffer, batch, and
parallelize the file-reconstruction step.

What does **not** change:

* Producers still write in order. The current senders all do,
  and the receiver still relies on EOF being the last chunk for
  a path to know the file is complete.
* Multi-producer parallel writes into one path are not legalised
  by this note — that would require a separate
  `PARTIAL_EOF` marker so each producer can signal "I'm done with
  my share" without claiming the whole file is closed. PARTIAL_EOF
  is out of scope for this release; the design is sketched in
  PXB-3754's design doc.

The wire format does not change — every writer in tree has always
populated the offset field correctly. The spec note simply documents
that readers may now use it for parallel `pwrite()` placement.

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

## Future: segments-as-files for files larger than the per-object cap

The xbstream protocol has no per-file size limit, but the cloud
backends it pipes into do (S3's 5 TiB per object, for example).
Today this is not handled in tree -- writing a 6 TiB file through
`--stream=xbstream | xbcloud put` would fail when xbcloud's last
chunk pushes the cumulative size past the cap. In the next release,
together with ds_cloud direct streaming, the producer will split
oversized files at write time into N segments named
`<path>.r1`, `<path>.r2`, ... and emit each segment as an
**independent xbstream file**:

```
producer:
  ds_xbstream open("test/big.ibd.r1")   ... PAYLOAD chunks ... EOF
  ds_xbstream open("test/big.ibd.r2")   ... PAYLOAD chunks ... EOF
```

No wire-format change is needed for this. Each `.rN` is a
complete xbstream file with its own PAYLOAD chunks (in offset
order within the segment) and its own EOF chunk. The xbstream
protocol sees N files, not one parallelised file.

The "these N segments compose one logical file" knowledge lives
above the protocol layer, in `backup_files.jsonl`'s per-file
`segments` block. See `manifest_format.md` for the schema and the
restore-time reassembly logic.

Why not PARTIAL_EOF? An earlier design conversation considered
introducing a `PARTIAL_EOF` chunk type plus a `part_id` field in
the header to allow N producers to write parts of one xbstream
file with explicit "I'm done with my share" signalling. That
mechanism is more flexible but also more invasive (wire format
extension, producer-side coordination, receiver-side tally) and we
don't need it for the rollover case: segments-as-files gets
per-segment parallelism with zero protocol change.

`PARTIAL_EOF` remains parked. It would only be needed if we ever
support multiple independent producer processes feeding into one
xbstream file with no shared coordination -- a use case we do not
have today. The sketch lives in PXB-3754's design comments for
future reference.
