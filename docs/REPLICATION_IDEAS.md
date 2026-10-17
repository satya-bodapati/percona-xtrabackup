# Replicating a single-table restore cleanly (v2 ideas)

Status: discussion notes, 2026-10-16, continued 2026-10-17 (A2 ordering, begin event, server emits the chunks).
Paused 2026-10-17 at the next open point: the binlog format of the chunks (new event types, like LOAD DATA's
`Append_block`, which older replicas and `mysqlbinlog` can't read, or existing event types).

v1 is decided: no binlogging. `--restore` runs with `sql_log_bin=0` and is run on each server.

## The real problem is ordering, not only the copy

In the common case the table was dropped on the source, and the DROP replicated, so it's gone on the replicas too.

- With v1, the source has the table back but the replicas don't.
- The first write the application makes to the restored table replicates and stops every replica with "table
  doesn't exist".
- If each replica restores from a different backup, or after writes have started, the servers diverge silently.

So v1 is safe only if every server restores the **same file content** before **any** write reaches the table.
The docs must say this.

A clean v2 makes the import happen at the **same point in every server's history**: a binlog position or a
GTID. That point should carry the data, or a reference to it, so nobody has to copy files by hand or hold
credentials for every server.

## Idea A: put the file in the binlog, in chunks (recommended)

MySQL already ships whole files through the binlog for statement-based `LOAD DATA`:
- `Begin_load_query_log_event`, `Append_block_log_event` and `Execute_load_query_log_event` (`sql/log_event.h:2068-2309`);
- the replica reassembles the blocks in `replica_load_tmpdir` (`sql/log_event.cc:713`) and then executes the statement.

Applied to IMPORT TABLE:
- The source logs one GTID transaction: begin, the file in chunks of about 1 MB, then
  `IMPORT TABLE FROM '<db>/<t>.ibd'`.
- The replica writes the chunks to a temp file, moves it into its own `<datadir>/<db>/`, and runs its own import.
  It assigns its own space id and index ids and converts the pages itself.
- **No key in the binlog.** The source sends the pages **decrypted**. The binlog already carries row data in plain
  text unless `binlog_encryption=ON`. The replica encrypts the file with its own keyring, so no `.cfp` or
  tablespace key is ever written to the binlog.
- **Pros:**
  - fully automatic;
  - no new connections and no credentials: the bytes use the replication connection the replica already has;
  - works with cascades, delayed replicas, GTID auto-position and multi-source;
  - point-in-time recovery works: replaying the binlog with `mysqlbinlog` re-imports the table at the right point.
- **Cost:** the binlog grows by the table size. That's the same as a logical reload such as mysqldump, but each
  replica applies it at file-import speed instead of inserting row by row.
- **Group Replication:** GR sends each transaction to every member as one message through its Paxos layer
  (XCom) before commit.
  - `group_replication_transaction_size_limit` (default 150,000,000 bytes, max 2 GB, 0 = no limit;
    `plugin_variables.h:235-236`) refuses larger transactions, but **only for DML** (`is_dml &&`,
    `observer_trans.cc:461`).
  - A DDL that carries the file wouldn't hit that check, but sending a large file as one Paxos message is still
    not workable, so GR needs idea B, fetching over the recovery channel.

## Idea A2: chunks as many small transactions (makes A work for GR too)

GR's problem with A is **one huge transaction**, not chunking itself. So split the file across transactions
instead of within one.

- **Chunk transactions.** Each chunk is its own small GTID transaction, for example 16 MB, always under
  `group_replication_transaction_size_limit`. It carries the import uuid, the byte offset and the bytes.
  - Every server, the source included, writes it at that offset into a staging file:
    `<datadir>/#import/<uuid>.stage`.
  - The extension is not `.ibd`, so InnoDB's startup file scan never sees a half-written file with a stale
    space id.
- **Final transaction.** `IMPORT TABLE ... FROM STAGED '<uuid>'`, carrying the file checksum, verifies the staged
  file, moves it to `<datadir>/<db>/<t>.ibd` and runs the normal import.
- **Ordering.** All chunk transactions and the final one use one shared writeset key, the import uuid. Then
  WRITESET dependency tracking and multi-threaded replicas keep the final transaction after every chunk. Writes
  at an offset don't depend on the order of the chunks anyway.
- **Who emits the chunks: the server (Satya, 2026-10-17).** Not xtrabackup. A client, xtrabackup or a user in the
  SQL client, only runs `IMPORT TABLE FROM '<db>/<t>.ibd'`, and the server does the rest: checks, begin, chunks,
  final. The same statement behaves the same way whoever runs it.
  - One statement on the source commits N+2 transactions, each with its own GTID. That works with
    `gtid_next=AUTOMATIC`. With `gtid_next` set to one GTID the statement is refused.
  - Replicas and `mysqlbinlog` replay the begin, chunk and final transactions, never the statement itself.
  - The name is reserved by the statement's own MDL, from the checks to the final commit. No lock needs to
    outlive a statement.
  - KILL or a disconnect mid-stream writes the abort event.
  - With `sql_log_bin=0` or the binlog off, nothing is shipped: a plain local import, as in v1.
- **Order on the source (agreed with Satya, 2026-10-17).**
  1. Checks first, with nothing in the binlog. If they fail, the statement fails and leaves no trace.
  2. Reserve the name: an exclusive MDL on `db.t`, held until the final transaction or the abort. Binlog a small
     **begin** transaction (agreed 2026-10-17) with the import uuid, `db.t`, the file size, the checksum and the
     SDI. Each replica runs the same checks there (schema exists, name free, keyring, disk space for the size), so
     a replica that can't take the table stops at the start with a clear error, not after the whole file arrived.
  3. Ship the chunks. Each chunk is read from the untouched original file and committed on its own.
  4. The final IMPORT commits last. Application writes to the table can only follow it in the binlog.

  Not after the import commits: application writes would then land among the chunks, and the source's import has
  already rewritten the file with its own ids and key. Not as one transaction at commit: copying it into the binlog
  would hold the binlog lock and stall every other commit.
- **Failure.** If the source dies mid-stream, every server holds a partial stage file. An explicit
  `IMPORT TABLE ABORT '<uuid>'` event, or a cleanup at startup of stages that never got a final transaction,
  removes it. A rerun uses a new uuid.
- **Pros:** everything A has (no credentials, PITR, cascades), and it also works with GR in single-primary mode:
  each message is small, flow control throttles the stream naturally, and a joining member recovers it from the
  binlog or gets the finished table through clone.
- **Cons:**
  - thousands of GTIDs per big table (100 GB at 16 MB is about 6,400);
  - the stage files need lifecycle handling;
  - the replica must encrypt the plain pages during import. The page converter already rewrites every page, so it
    could encrypt them on the way.

## Idea B: binlog "attachments" fetched over the replication connection

Same as A, but the bytes stay out of the binlog files.
- The event holds a reference: attachment id, size and checksum.
- The file is kept next to the binlog (for example `binlog.000123.att/1.ibd`) and is purged together with that
  binlog file, so the retention rules don't change.
- The replica's receiver thread fetches the attachment over the same connection and credentials it already uses.
  An intermediate replica with `log_replica_updates` keeps its own copy for the servers downstream of it.
- **Pros:** binlog files stay small, and it fits GR through the recovery channel, whose credentials already exist
  per member.
- **Cons:**
  - a new protocol command;
  - `mysqlbinlog` and PITR need the attachments next to the binlog;
  - more server work than A.

## Idea C: an ordering marker, with each host placing the file itself

- The source logs only a marker: `IMPORT TABLE db.t` plus the backup's identity (backup uuid and a file checksum).
- At that point the replica's applier waits in a clear state ("Waiting for import file db/t.ibd, checksum X"),
  instead of failing with ER_FILE_NOT_FOUND as IMPORT TABLESPACE does today.
- On each replica host, the operator runs `xtrabackup --restore --for-replica`, or a similar mode, with **local**
  credentials. The applier checks the checksum, imports and continues.
- **Pros:** little server work; ordering and "same content everywhere" are guaranteed, because a mismatched
  checksum is refused.
- **Cons:** it's still a manual step on every host, and each one needs access to the backup (NFS or xbcloud).
  This is today's IMPORT TABLESPACE flow made safe, not made automatic.

## Idea D: binlog rows (logical)

- After the import, the source scans the table and logs it as `Write_rows` events, as a logical reload would.
- **Pros:** works with any replica, including upstream MySQL, other engines, other versions and GR (within the size
  limit). Little new code.
- **Cons:** applying row by row on every replica is slow for big tables, and the binlog cost is the same as A.
- Could be the fallback for small tables, under a size threshold.

## Idea E (future, Satya): the table on S3, every server downloads it

The binlog carries only a reference: an object URL plus a checksum. Each server downloads the files and imports
them at that point in its history. This is idea B with the shared store being S3 instead of the source's disk.

- **The .ibd is not enough.** Some state lives outside it, so the unit must be a **bundle**:
  - every partition `.ibd` and the FTS aux `fts_*.ibd` files;
  - a small manifest (JSON) with the triggers (from `mysql.triggers`), the AUTO_INCREMENT counter (from
    `innodb_dynamic_metadata`), the schema defaults for CREATE DATABASE, the SDI version, and a checksum for
    each file;
  - histograms, if we care (from `mysql.column_statistics`). Persistent stats are recomputed on import anyway.
- **Split by size.** The manifest is tiny, so it can ride **inside the binlog event**. Only the large bytes go to
  S3.
- **Build it early.** The same manifest helps v1 too. Built at prepare time, or by xbcloud at upload time, it makes
  each table in a backup self-contained, so a later restore doesn't have to parse the backup's `mysql.ibd`.
- **Credentials.** In the cloud, one bucket policy plus instance roles scales better than per-server MySQL
  credentials. On-prem it's yet another credential to distribute.
- **Who downloads.** The server has no S3 client; xbcloud does. Either:
  - a server component that downloads (new code in the server), or
  - the idea C wait state, plus an xtrabackup agent on each host that sees the marker, downloads, verifies and
    places the files.
- **Encryption.** Keep the pages encrypted in S3 with a per-bundle key. The key can't go into the binlog in
  plain text, so it is wrapped by a key every server can unwrap (KMS or keyring), or the bundle uses xbcloud's
  existing xbcrypt encryption.
- **Retention.** The objects must outlive the longest replica lag, any delayed replica, and the PITR window, like
  binlog retention. S3 object lock or versioning keeps them immutable.

## Recommendation

- **A2**, chunks as many small transactions, for both asynchronous replication and GR. It keeps A's LOAD DATA-style
  shipping and handles the user-friendliness and credentials problems, without one giant transaction.
- **B later**, if binlog volume becomes the blocker.
- **C** is the cheapest step that still removes silent divergence, if v2 must be small.
- **D** only as a small-table fallback.
