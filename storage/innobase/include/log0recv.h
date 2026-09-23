/*****************************************************************************

Copyright (c) 1997, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/log0recv.h
 Recovery

 Created 9/20/1997 Heikki Tuuri
 *******************************************************/

#ifndef log0recv_h
#define log0recv_h

#include "buf0types.h"
#include "dict0types.h"
#include "hash0hash.h"
#include "log0sys.h"
#include "mtr0types.h"

/* OS_FILE_LOG_BLOCK_SIZE */
#include "os0file.h"

#include "ut0byte.h"
#include "ut0new.h"

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

class MetadataRecover;
class PersistentTableMetadata;

struct recv_addr_t;

#ifdef XTRABACKUP
/** map of tablespace_id, that would be full scan during backup with
 * pagetracking*/
extern std::unordered_set<space_id_t> full_scan_tables;

/* count of tablespaces that will require full table scan. This is calculated
 * after full_scan_tables is fully populated and should never changed. */

extern size_t full_scan_tables_count;

namespace xtrabackup {
struct recv_sys_t {
  struct mem_block_t {
    /** Constructor
    @param[in]	n	size of this block and total size of all the blocks. */
    mem_block_t(ulint n) : len(n), total_size(n), free(MEM_BLOCK_HEADER_SIZE) {}

    /** Default constructor */
    mem_block_t() : len(), total_size(), free(MEM_BLOCK_HEADER_SIZE) {}
    ulint len;
    ulint total_size;
    ulint free;
  };

  struct space_page_t {
    /** Default constructor */
    space_page_t() : m_pages(), m_blocks() {}
    std::unordered_set<page_no_t> m_pages;
    std::vector<mem_block_t *> m_blocks;
  };

  using Spaces =
      std::unordered_map<space_id_t, space_page_t *, std::hash<space_id_t>,
                         std::equal_to<space_id_t>>;
  Spaces *spaces;
  ~recv_sys_t() {
    if (this->spaces != nullptr) {
      for (auto &space : *this->spaces) {
        for (auto &block : space.second->m_blocks) {
          delete (block);
        }
        delete (space.second);
      }
      ut::delete_(this->spaces);
    }
  }
};

/** calculates the amount of memory required for xtrabackup prepare
@retval amount of memory required for hash table of parsed records
@retval number of database frames */
std::pair<size_t, ulint> recv_backup_heap_used();
}  // namespace xtrabackup

#endif /* XTRABACKUP */

/** list of tablespaces, that experienced an inplace DDL during a backup op */
extern std::list<std::pair<space_id_t, lsn_t>> index_load_list;
/** the last redo log flush len as seen by MEB */
extern volatile lsn_t backup_redo_log_flushed_lsn;
/** true when the redo log is being backed up */
extern bool recv_is_making_a_backup;

#ifdef UNIV_HOTBACKUP

/** Scans the log segment and n_bytes_scanned is set to the length of valid
log scanned.
@param[in]      buf                     buffer containing log data
@param[in]      buf_len                 data length in that buffer
@param[in,out]  scanned_lsn             lsn of buffer start, we return scanned
lsn
@param[in,out]  scanned_checkpoint_no   4 lowest bytes of the highest scanned
@param[out]     block_no        highest block no in scanned buffer.
checkpoint number so far
@param[out]     n_bytes_scanned         how much we were able to scan, smaller
than buf_len if log data ended here
@param[out]     has_encrypted_log       set true, if buffer contains encrypted
redo log, set false otherwise */
void meb_scan_log_seg(byte *buf, size_t buf_len, lsn_t *scanned_lsn,
                      uint32_t *scanned_checkpoint_no, uint32_t *block_no,
                      size_t *n_bytes_scanned, bool *has_encrypted_log);

/** Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.


@param[in,out]  block           buffer block */
void recv_recover_page_func(buf_block_t *block);

/** Wrapper for recv_recover_page_func().
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.
@param jri in: true if just read in (the i/o handler calls this for
a freshly read page)
@param block in,out: the buffer block
*/
static inline void recv_recover_page(bool jri [[maybe_unused]],
                                     buf_block_t *block) {
  recv_recover_page_func(block);
}

/** Applies log records in the hash table to a backup. */
void meb_apply_log_recs(void);

/** Applies log records in the hash table to a backup using a callback
functions.
@param[in]      apply_log_record_function  function for apply
@param[in]      wait_till_done_function    function for wait */
void meb_apply_log_recs_via_callback(
    void (*apply_log_record_function)(recv_addr_t *),
    void (*wait_till_done_function)());

/** Applies a log record in the hash table to a backup.
@param[in]      recv_addr       chain of log records
@param[in,out]  block           buffer block to apply the records to */
void meb_apply_log_record(recv_addr_t *recv_addr, buf_block_t *block);

/** Process a file name passed as an input
@param[in]      name            absolute path of tablespace file
@param[in]      space_id        the tablespace ID
@retval         true            if able to process file successfully.
@retval         false           if unable to process the file */
void meb_fil_name_process(const char *name, space_id_t space_id);

/** Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.  Unless
UNIV_HOTBACKUP is defined, this function will apply log records
automatically when the hash table becomes full.
@param[in]      available_memory        we let the hash table of recs
to grow to this size, at the maximum
@param[in]      buf                     buffer containing a log
segment or garbage
@param[in]      len                     buffer length
@param[in]      start_lsn               buffer start lsn
@param[out]     group_scanned_lsn       scanning succeeded up to this lsn
@retval	true  if limit_lsn has been reached, or not able to scan any
more in this log group
@retval false   otherwise */
bool meb_scan_log_recs(size_t available_memory, const byte *buf, size_t len,
                       lsn_t start_lsn, lsn_t *group_scanned_lsn);

/** Check the 4-byte checksum to the trailer checksum field of a log
block.
@param[in]      block   pointer to a log block
@return whether the checksum matches */
bool log_block_checksum_is_ok(const byte *block);
#else /* UNIV_HOTBACKUP */

/** Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.


@param[in]      just_read_in    true if the IO handler calls this for a freshly
                                read page
@param[in,out]  block           buffer block */
void recv_recover_page_func(bool just_read_in, buf_block_t *block);

/** Wrapper for recv_recover_page_func().
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.
@param jri in: true if just read in (the i/o handler calls this for
a freshly read page)
@param[in,out]  block   buffer block */
static inline void recv_recover_page(bool jri, buf_block_t *block) {
  recv_recover_page_func(jri, block);
}

#endif /* UNIV_HOTBACKUP */

/** Frees the recovery system. */
void recv_sys_free();

/** Reset the state of the recovery system variables. */
void recv_sys_var_init();

#ifdef UNIV_HOTBACKUP
/** Get the number of bytes used by all the heaps
@return number of bytes used */
size_t meb_heap_used();
#endif /* UNIV_HOTBACKUP */

/** Returns true if recovery is currently running.
@return recv_recovery_on */
[[nodiscard]] static inline bool recv_recovery_is_on();

/** Returns true if the page is brand new (the next log record is init_file_page
or no records to apply).
@param[in]      block           buffer block
@return true if brand new */
bool recv_page_is_brand_new(buf_block_t *block);

/** Start recovering from a redo log checkpoint.
@see recv_recovery_from_checkpoint_finish
@param[in,out]  log        redo log
@param[in]      flush_lsn  lsn stored at offset FIL_PAGE_FILE_FLUSH_LSN
                           in the system tablespace header
@param[in]      to_lsn          LSN to store recovery at
@return error code or DB_SUCCESS */
[[nodiscard]] dberr_t recv_recovery_from_checkpoint_start(log_t &log,
                                                          lsn_t flush_lsn,
                                                          lsn_t to_lsn);

/** Determine if a redo log from a version before MySQL 8.0.30 is clean.
@param[in,out]  log             redo log
@return error code
@retval DB_SUCCESS  if the redo log is clean
@retval DB_ERROR    if the redo log is corrupted or dirty */
dberr_t recv_verify_log_is_clean_pre_8_0_30(log_t &log);

/** Complete the recovery from the latest checkpoint.
@param[in]      aborting        true if the server has to abort due to an error
@return recovered persistent metadata or nullptr if aborting*/
[[nodiscard]] MetadataRecover *recv_recovery_from_checkpoint_finish(
    bool aborting);

/** Creates the recovery system. */
void recv_sys_create();

/** Release recovery system mutexes. */
void recv_sys_close();

/** Inits the recovery system for a recovery operation. */
void recv_sys_init();

/** Calculates the new value for lsn when more data is added to the log.
@param[in]      lsn             Old LSN
@param[in]      len             This many bytes of data is added, log block
                                headers not included
@return LSN after data addition */
lsn_t recv_calc_lsn_on_data_add(lsn_t lsn, os_offset_t len);

/** Empties the hash table of stored log records, applying them to appropriate
pages.
@param[in,out]  log             redo log
@param[in]      allow_ibuf      if true, ibuf operations are allowed during
                                the application; if false, no ibuf operations
                                are allowed, and after the application all
                                file pages are flushed to disk and invalidated
                                in buffer pool: this alternative means that
                                no new log records can be generated during
                                the application; the caller must in this case
                                own the log mutex */
void recv_apply_hashed_log_recs(log_t &log, bool allow_ibuf);

#if defined(UNIV_DEBUG) || defined(UNIV_HOTBACKUP)
/** Return string name of the redo log record type.
@param[in]      type    record log record enum
@return string name of record log record */
const char *get_mlog_string(mlog_id_t type);
#endif /* UNIV_DEBUG || UNIV_HOTBACKUP */

/** Block of log record data */
struct recv_data_t {
  /** pointer to the next block or NULL.  The log record data
  is stored physically immediately after this struct, max amount
  RECV_DATA_BLOCK_SIZE bytes of it */

  recv_data_t *next;
};

/** Stored log record struct.

One of these exists for every redo record recovery holds, and the recovery
heap is what decides how many apply batches --prepare has to run: when the
heap hits its bound the batch fires, the buffer pool is invalidated, and
every page named in a later batch is read again. So the width of this
struct, not the redo volume, is what sets the batch count on a typical OLTP
log where bodies are 8-35 bytes. It is packed accordingly, and the body of
a record short enough to fit one chunk -- which is nearly all of them --
lives inline immediately after the struct instead of behind a recv_data_t.
That is 40 bytes of overhead per record where it used to be 64. */
struct recv_t {
  using Node = UT_LIST_NODE_T(recv_t);

  /** Start lsn of the log segment written by the mtr which generated
  this log record: NOTE that this is not necessarily the start lsn of
  this log record */
  lsn_t start_lsn;

  /** List node, list anchored in recv_addr_t */
  Node rec_list;

  /** Log record body length in bytes */
  uint32_t len;

  /** end_lsn - start_lsn of the mtr that generated this record. An mtr
  never spans 4 GiB of LSN, so the delta fits where the absolute value did
  not. */
  uint32_t end_delta;

  /** Log record type. Every mlog_id_t value fits in a byte. */
  uint8_t type_id;

  /** Nonzero when the body is stored inline directly after this struct;
  zero when it is a recv_data_t chain whose head is stored there instead. */
  uint8_t body_inline;

  mlog_id_t type() const { return static_cast<mlog_id_t>(type_id); }

  /** End lsn of the log segment written by the mtr which generated
  this log record: NOTE that this is not necessarily the end LSN of
  this log record */
  lsn_t end_lsn() const { return start_lsn + end_delta; }

  /** @return the inline body, valid only when body_inline and len > 0 */
  byte *inline_body() { return reinterpret_cast<byte *>(this + 1); }

  /** @return head of the body chain, valid only when !body_inline */
  recv_data_t *chain() const {
    return *reinterpret_cast<recv_data_t *const *>(this + 1);
  }
  void set_chain(recv_data_t *d) {
    *reinterpret_cast<recv_data_t **>(this + 1) = d;
  }
};

/** States of recv_addr_t */
enum recv_addr_state {

  /** not yet processed */
  RECV_NOT_PROCESSED,

  /** page is being read */
  RECV_BEING_READ,

  /** log records are being applied on the page */
  RECV_BEING_PROCESSED,

  /** log records have been applied on the page */
  RECV_PROCESSED,

  /** log records have been discarded because the tablespace
  does not exist */
  RECV_DISCARDED
};

/** Hashed page file address struct */
/** A run of packed redo records for one page.

Records are appended in ascending LSN order and read back once, forward, so
they are stored byte-packed one after another rather than as a linked list of
structs. See Page_recs in log0recv.cc for the encoding; nothing outside it
may read data().

A chunk is self-contained: the first record's LSN is base_lsn and every
record stores its LSN as a delta from the one before it in THIS chunk. That
is what lets the parallel merge concatenate chunk lists from different
workers without rewriting anything. */
struct Rec_chunk {
  /** Next chunk of the same page, or nullptr. */
  Rec_chunk *next;

  /** start_lsn of the first record in this chunk. */
  lsn_t base_lsn;

  /** Bytes of data() in use. */
  uint32_t used;

  /** Bytes of data() available. */
  uint32_t cap;

  byte *data() { return reinterpret_cast<byte *>(this + 1); }
  const byte *data() const { return reinterpret_cast<const byte *>(this + 1); }
};

struct recv_addr_t {
  /** recovery state of the page */
  recv_addr_state state;

  /** Space ID */
  space_id_t space;

  /** Page number */
  page_no_t page_no;

  /** Packed log records for this page, oldest chunk first. */
  Rec_chunk *chunk_head;

  /** Last chunk, so append and the merge are O(1). */
  Rec_chunk *chunk_tail;

  /** start_lsn of the last record appended, so the next append can store a
  delta and so the ascending-LSN invariant the encoding depends on can be
  asserted rather than assumed. */
  lsn_t last_lsn;
};

// Forward declaration
namespace dblwr {
namespace recv {
class DBLWR;
}
}  // namespace dblwr

/** Class to parse persistent dynamic metadata redo log, store and
merge them and apply them to in-memory table objects finally */
class MetadataRecover {
  using PersistentTables = std::map<
      table_id_t, PersistentTableMetadata *, std::less<table_id_t>,
      ut::allocator<std::pair<const table_id_t, PersistentTableMetadata *>>>;

 public:
  /** Default constructor */
  MetadataRecover() UNIV_NOTHROW = default;

  /** Destructor */
  ~MetadataRecover();

  /** Parse a dynamic metadata redo log of a table and store
  the metadata locally
  @param[in]    id      table id
  @param[in]    version table dynamic metadata version
  @param[in]    ptr     redo log start
  @param[in]    end     end of redo log
  @retval ptr to next redo log record, nullptr if this log record
  was truncated */
  const byte *parseMetadataLog(table_id_t id, uint64_t version, const byte *ptr,
                               const byte *end);

  /** Store the collected persistent dynamic metadata to
  mysql.innodb_dynamic_metadata */
  void store();

  /** If there is any metadata to be applied
  @return       true if any metadata to be applied, otherwise false */
  bool empty() const { return m_tables.empty(); }

 private:
  /** Get the dynamic metadata of a specified table,
  create a new one if not exist
  @param[in]    id      table id
  @return the metadata of the specified table */
  PersistentTableMetadata *getMetadata(table_id_t id);

 private:
  /** Map used to store and merge persistent dynamic metadata */
  PersistentTables m_tables;
};

/** Recovery system data structure */
struct recv_sys_t {
  using Pages =
      std::unordered_map<page_no_t, recv_addr_t *, std::hash<page_no_t>,
                         std::equal_to<page_no_t>>;

  /** Every space has its own heap and pages that belong to it. */
  struct Space {
    /** Constructor
    @param[in,out]      heap    Heap to use for the log records. */
    explicit Space(mem_heap_t *heap) : m_heap(heap), m_pages() {}

    /** Default constructor */
    Space() : m_heap(), m_pages() {}

    /** Memory heap of log records and file addresses */
    mem_heap_t *m_heap;

    /** Pages that need to be recovered */
    Pages m_pages;
  };

  using Missing_Ids = std::set<space_id_t>;

  using Spaces = std::unordered_map<space_id_t, Space, std::hash<space_id_t>,
                                    std::equal_to<space_id_t>>;

  /* Recovery encryption information */
  struct Encryption_Key {
    /** Tablespace ID */
    space_id_t space_id;

    /** LSN of REDO log encryption entry */
    lsn_t lsn;

    /** Encryption key */
    byte *ptr;

    /** Encryption IV */
    byte *iv;
  };

  using Encryption_Keys = std::vector<Encryption_Key>;

  /** Mini transaction log record. */
  struct Mlog_record {
    /* Space ID */
    space_id_t space_id;
    /* Page number */
    page_no_t page_no;
    /* Log type */
    mlog_id_t type;
    /* Log body */
    const byte *body;
    /* Record size */
    size_t size;
  };

  using Mlog_records = std::vector<Mlog_record, ut::allocator<Mlog_record>>;

  /** While scanning logs for multi-record mini-transaction (mtr), we have two
  passes. In first pass, we check if all the logs of the mtr is present in
  current recovery buffer or not. If yes, then in second pass we go through the
  logs again the add to hash table for apply. To avoid parsing multiple times,
  we save the parsed records in first pass and reuse them in second pass.

  Parsing of redo log takes significant amount of time and this optimization of
  avoiding second parse gave about 1.8x speed up on recovery scan time of 1G of
  redo log from sysbench rw test.

  There is currently no limit for maximum number of logs in an mtr. Practically,
  from sysbench rw test recovery with 1G of redo log to recover from the record
  count were spread from 3 - 1235 with majority between 600 - 700. So, it is
  likely by saving 1k records we could avoid most of the re-parsing overhead.
  Considering possible bigger number of records in other load and future changes
  the limit for number of saved records is kept at 8k. The same value from the
  contribution patch. The memory requirement 32 x 8k = 256k seems fine as one
  time overhead for the entire instance.  */
  static constexpr size_t MAX_SAVED_MLOG_RECS = 8 * 1024;

  /** Save mlog record information. Silently returns if cannot save. Works only
  in single threaded recovery scanner.
  @param[in]    rec_num         record number in multi record group
  @param[in]    space_id        space ID for the log record
  @param[in]    page_no         page number for the log record
  @param[in]    type            log record type
  @param[in]    body            pointer to log record body in recovery buffer
  @param[in]    len             length of the log record */
  void save_rec(size_t rec_num, space_id_t space_id, page_no_t page_no,
                mlog_id_t type, const byte *body, size_t len) {
    /* No more space to save log. */
    if (rec_num >= MAX_SAVED_MLOG_RECS) {
      return;
    }

    ut_ad(rec_num < saved_recs.size());

    if (rec_num >= saved_recs.size()) {
      return;
    }

    auto &saved_rec = saved_recs[rec_num];

    saved_rec.space_id = space_id;
    saved_rec.page_no = page_no;
    saved_rec.type = type;
    saved_rec.body = body;
    saved_rec.size = len;
  }

  /** Return saved mlog record information, if there. Works only
  in single threaded recovery scanner.
  @param[in]    rec_num         record number in multi record group
  @param[out]   space_id        space ID for the log record
  @param[out]   page_no         page number for the log record
  @param[out]   type            log record type
  @param[out]   body            pointer to log record body in recovery buffer
  @param[out]   len             length of the log record
  @return true iff saved record data is found. */
  bool get_saved_rec(size_t rec_num, space_id_t &space_id, page_no_t &page_no,
                     mlog_id_t &type, const byte *&body, size_t &len) {
    if (rec_num >= MAX_SAVED_MLOG_RECS) {
      return false;
    }

    ut_ad(rec_num < saved_recs.size());

    if (rec_num >= saved_recs.size()) {
      return false;
    }

    auto &saved_rec = saved_recs[rec_num];

    space_id = saved_rec.space_id;
    page_no = saved_rec.page_no;
    type = saved_rec.type;
    body = const_cast<byte *>(saved_rec.body);
    len = saved_rec.size;

    return true;
  }

#ifndef UNIV_HOTBACKUP

  /*!< mutex protecting the fields apply_log_recs, n_addrs, and the
  state field in each recv_addr struct */
  ib_mutex_t mutex;

  /** mutex coordinating flushing between recv_writer_thread and
  the recovery thread. */
  ib_mutex_t writer_mutex;

  /** event to activate page cleaner threads */
  os_event_t flush_start;

  /** event to signal that the page cleaner has finished the request */
  os_event_t flush_end;

  /** type of the flush request. BUF_FLUSH_LRU: flush end of LRU,
  keeping free blocks.  BUF_FLUSH_LIST: flush all of blocks. */
  buf_flush_t flush_type;

#else  /* !UNIV_HOTBACKUP */
  bool apply_file_operations;
#endif /* !UNIV_HOTBACKUP */

  /** This is true when log rec application to pages is allowed;
  this flag tells the i/o-handler if it should do log record
  application */
  bool apply_log_recs;

  /** This is true when a log rec application batch is running */
  bool apply_batch_on;

  /** Buffer for parsing log records */
  byte *buf;

  /** Size of the parsing buffer */
  size_t buf_len;

  /** Amount of data in buf */
  ulint len;

  /** This is the lsn from which we were able to start parsing
  log records and adding them to the hash table; zero if a suitable
  start point not found yet */
  lsn_t parse_start_lsn;

  /** Checkpoint lsn that was used during recovery (read from file). */
  lsn_t checkpoint_lsn;

  /** Number of data bytes to ignore until we reach checkpoint_lsn. */
  ulint bytes_to_ignore_before_checkpoint;

  /** The log data has been scanned up to this lsn */
  lsn_t scanned_lsn;

  /** The log data has been scanned up to this epoch_no */
  uint32_t scanned_epoch_no;

  /** Start offset of non-parsed log records in buf */
  ulint recovered_offset;

  /** The log records have been parsed up to this lsn */
  lsn_t recovered_lsn;

  /** The previous value of recovered_lsn - before we parsed the last mtr.
  It is equal to recovered_lsn before we parsed any mtr. This is used to
  find moments in which recovered_lsn moves to the next block in which case
  we should update the last_block_first_rec_group (described below). */
  lsn_t previous_recovered_lsn;

  /** Tracks what should be the proper value of first_rec_group field in the
  header of the block to which recovered_lsn belongs. It might be also zero,
  in which case it means we do not know. */
  lsn_t last_block_first_mtr_boundary{};

  /** Set when finding a corrupt log block or record, or there
  is a log parsing buffer overflow */
  bool found_corrupt_log;

  /** Set when an inconsistency with the file system contents
  is detected during log scan or apply */
  bool found_corrupt_fs;

  /** Data directory has been recognized as cloned data directory. */
  bool is_cloned_db;

  /** Data directory has been recognized as data directory from MEB. */
  bool is_meb_db;

  /** Doublewrite buffer state before MEB recovery starts. We restore to this
  state after MEB recovery completes and disable the doublewrite buffer during
  MEB recovery. */
  bool dblwr_state;

  /** Hash table of pages, indexed by SpaceID. */
  Spaces *spaces;

  /** Number of not processed hashed file addresses in the hash table */
  ulint n_addrs;

  /** Doublewrite buffer pages, destroyed after recovery completes */
  dblwr::recv::DBLWR *dblwr;

  /** We store and merge all table persistent data here during
  scanning redo logs */
  MetadataRecover *metadata_recover;

  /** Encryption Key information per tablespace ID */
  Encryption_Keys *keys;

  /** Tablespace IDs that were ignored during redo log apply. */
  Missing_Ids missing_ids;

  /** Tablespace IDs that were explicitly deleted. */
  Missing_Ids deleted;

  /* Saved log records to avoid second round parsing log. */
  Mlog_records saved_recs;
};

/** The recovery system */
extern recv_sys_t *recv_sys;

/** true when applying redo log records during crash recovery; false
otherwise.  Note that this is false while a background thread is
rolling back incomplete transactions. */
extern volatile bool recv_recovery_on;

/** If the following is true, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes true if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

true means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
extern bool recv_no_ibuf_operations;

/** true when recv_init_crash_recovery() has been called. */
extern bool recv_needed_recovery;

/** true if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially false, and set by
recv_recovery_from_checkpoint_start(). */
extern bool recv_lsn_checks_on;

/** Does this redo record still need to be applied to a page whose current
LSN is page_lsn?

This is THE comparison that decides whether a redo record is already contained
in a page. It is deliberately a single shared function: recv_recover_page_func()
makes this test after reading the page, and the page LSN map makes the same test
before the page is read, using the LSN recorded at backup copy time. If the two
sites ever disagreed, the map would drop records that apply -- a stale page that
still passes its own checksum, i.e. silent corruption.

Note the inequality is NOT strict. A record whose start_lsn exactly equals the
page LSN belongs to the mtr that begins where the page's last modification
ended, so its changes are NOT yet in the page and it must be applied.
@param[in]      record_lsn      start_lsn of the redo record
@param[in]      page_lsn        FIL_PAGE_LSN of the page
@return true if the record must be applied */
[[nodiscard]] static inline bool xb_redo_record_applies(lsn_t record_lsn,
                                                        lsn_t page_lsn) {
  return record_lsn >= page_lsn;
}

#ifdef XTRABACKUP
/** Counters and timers for a single xtrabackup --prepare run. Bumped from
log0recv.cc (which never prints) and reported as one machine-parseable
XB-PREPARE-STATS line by xtrabackup.cc at the end of prepare.

recv_recover_page_func() runs on the I/O completion threads, so the counters it
touches are atomic; relaxed ordering is enough because nothing branches on them
and they are only read after recovery has quiesced. */
struct xb_recv_stats_t {
  /** apply batches run, and how many of those invalidated the buffer pool
  (the allow_ibuf == false ones, which are the expensive kind) */
  std::atomic<uint64_t> batches{0};
  std::atomic<uint64_t> batches_invalidating{0};
  /** tablespace pages submitted for read by recv_read_in_area() */
  std::atomic<uint64_t> pages_read{0};
  /** calls into recv_recover_page_func(), i.e. pages actually visited */
  std::atomic<uint64_t> page_applies{0};
  /** pages visited for which NOT ONE held record still applied -- the read was
  pure cost and is exactly what the page LSN map removes */
  std::atomic<uint64_t> pages_wasted{0};
  /** individual records applied vs already contained in the page */
  std::atomic<uint64_t> recs_applied{0};
  std::atomic<uint64_t> recs_superseded{0};
  /** Why a superseded record survived the map filter. Classified once per
  page at apply, then attributed to all of that page's superseded records.
  sup_no_entry: the map has no copy_lsn for the page, so nothing was dropped
  and the page's own FIL_PAGE_LSN did the filtering -- pure coverage gap.
  sup_page_ahead: the map HAS an entry but the page in the pool is already
  past it, i.e. an earlier batch applied to it, so the filter could not have
  known. sup_unexplained: entry present and page not ahead. Measured at
  312,885, and they are MLOG_INIT_FILE_PAGE/_PAGE2, which the filter
  deliberately never drops whatever their LSN, so they reach apply holding
  a perfectly good map entry and are superseded there. Expected. */
  std::atomic<uint64_t> sup_no_entry{0};
  std::atomic<uint64_t> sup_page_ahead{0};
  std::atomic<uint64_t> sup_unexplained{0};
  /** the same three, counted per page rather than per record */
  std::atomic<uint64_t> sup_pages_no_entry{0};
  std::atomic<uint64_t> sup_pages_page_ahead{0};
  /** sup_no_entry split further: is the whole tablespace missing from the
  map, or does the map hold the space but stop short of this page? */
  std::atomic<uint64_t> sup_space_absent{0};
  std::atomic<uint64_t> sup_page_past_end{0};
  std::atomic<uint64_t> sup_page_inside_range{0};

  /* Order-independent digest of every record the scan files, so the
  parallel parse can be compared against the serial one DIRECTLY rather
  than through a prepared datadir. Each record contributes
  h = crc32(space, page, start_lsn, end_lsn, type, len, body); the three
  accumulators below are a multiset fingerprint -- count catches an added
  or dropped record, sum catches a changed one, and sum of squares makes
  a compensating pair of changes vanishingly unlikely. Summation is
  commutative, so workers can accumulate concurrently and in any order,
  which is the point: the digest must not care who parsed what, only that
  the same set of records came out. Ordering WITHIN a page is checked
  separately, by assertion at apply. */
  std::atomic<uint64_t> filed_digest_n{0};
  std::atomic<uint64_t> filed_digest_sum{0};
  std::atomic<uint64_t> filed_digest_sq{0};

  /** Per-tablespace tally of the records the map failed to drop. The gap
  is concentrated -- roughly 15,000 pages carry 294.6M records, about
  10,000 each -- so naming the spaces is what turns "missing coverage"
  into a fix. Guarded by its own mutex and touched once per page visit,
  not once per record. */
  std::mutex sup_by_space_mutex;
  std::map<space_id_t, uint64_t> sup_by_space;
  /** records dropped before entering the hash because the map said so */
  std::atomic<uint64_t> recs_dropped_by_map{0};
  /** pages never entered into the hash at all because the map dropped every
  record for them -- these are the reads that never happened */
  std::atomic<uint64_t> pages_skipped_by_map{0};
  /** redo bytes read from the log file by the scan */
  std::atomic<uint64_t> redo_scan_bytes{0};
  /** high water mark of recv_heap_used(), i.e. why the batches fired */
  std::atomic<uint64_t> heap_max_bytes{0};
  /** records actually filed into the hash, and the total size of their
  bodies. Their ratio bounds what any scheme that moves bodies out of the
  heap could ever buy: the cost per record is a fixed ~56 byte recv_t plus
  the body, so removing the body only helps in proportion to b/(r+b). */
  std::atomic<uint64_t> recs_filed{0};
  std::atomic<uint64_t> body_bytes_filed{0};
  /** log2 histogram of filed record body sizes. The mean hides the shape:
  a small tail of large records can hold most of the bytes. */
  std::atomic<uint64_t> body_size_hist[16]{};
  /** histogram of records-per-page, log2 buckets 0..15, sampled at apply.
  The page LSN map's payoff is E[1/(K+1)] over this distribution and is
  capped at 50%, so this is the number that decides whether the map is
  worth its maintenance cost. */
  std::atomic<uint64_t> recs_per_page_hist[16]{};
  /** nanoseconds in the scan loop and inside apply batches */
  std::atomic<uint64_t> scan_ns{0};
  std::atomic<uint64_t> apply_ns{0};
  /** Decomposition of the end-of-batch tail, which is neither scanning nor
  applying and has repeatedly been mis-attributed by inspection. At
  --use-memory=32G, recovery_ms minus scan_ms minus apply_ms leaves 24.4s
  unaccounted; these say where it goes. Totals across all batches. */
  /** Apply-thread accounting, summed across all apply threads and batches.
  The apply phase uses about 40 of 128 cores and writes 0.8-1.4 GB/s where
  the same device sustains 2.5-3.3, so neither CPU nor disk is saturated and
  the threads must be waiting. External samplers cannot see this: the build
  has no frame pointers, so bcc resolves user stacks as [unknown]. The driver
  is our own code, so it can time itself.
  busy_ns is wall summed over threads; busy_ns / batch_wall is the effective
  core count actually achieved. */
  std::atomic<uint64_t> apply_busy_ns{0};
  std::atomic<uint64_t> apply_page_get_ns{0};
  std::atomic<uint64_t> apply_recover_ns{0};
  std::atomic<uint64_t> apply_mutex_ns{0};
  std::atomic<uint64_t> apply_items{0};
  std::atomic<uint64_t> await_no_flush_ns{0};
  std::atomic<uint64_t> flush_list_ns{0};
  std::atomic<uint64_t> invalidate_ns{0};
  /* Time in recv_sys_empty_hash(): the per-batch teardown of every space
  heap, the parscan staging heaps and both Spaces maps. */
  std::atomic<uint64_t> empty_hash_ns{0};
  /* Inside the scan: time filling the window from disk vs time parsing it.
  These alternate today, so scan_ms is their sum. */
  std::atomic<uint64_t> win_read_ns{0};
  std::atomic<uint64_t> win_parse_ns{0};
  /* Packed-record chunk accounting: bytes handed out by the heap versus
  bytes actually holding records. The difference is slack in the last chunk
  of each page. */
  std::atomic<uint64_t> chunk_bytes_alloc{0};
  std::atomic<uint64_t> chunk_bytes_used{0};
  std::atomic<uint64_t> chunks_made{0};
  /* Field-width census for the packed recv_t work. log2 buckets of the mtr
  LSN span (end_lsn - start_lsn), and a count of bodies too long to ride
  inline, which are the records that force the wide encoding. */
  std::atomic<uint64_t> end_delta_hist[16]{};
  std::atomic<uint64_t> recs_chained{0};
  /* log2 buckets of the LSN gap between consecutive records of the SAME
  page, sampled where apply walks them in order. This is the width of the
  only field of a packed recv_t that cannot be bounded a priori. */
  std::atomic<uint64_t> lsn_gap_hist[16]{};
};

extern xb_recv_stats_t xb_recv_stats;

/** Record one page's records-per-page count into the histogram. */
void xb_recv_stats_note_page(uint64_t n_recs);

/** Record one filed record's body length. */
void xb_recv_stats_note_body(uint64_t len);
void xb_recv_stats_note_widths(uint64_t end_delta, bool chained);
bool xb_recv_census_on();

/** Fold one filed record into the scan digest. Called from both the serial
and the parallel filing paths, which is the whole point. */
void xb_recv_note_filed(uint32_t space_id, uint32_t page_no, uint64_t start_lsn,
                        uint64_t end_lsn, int type, uint32_t len,
                        const unsigned char *body);
#endif /* XTRABACKUP */

/** Size of the parsing buffer; it must accommodate RECV_SCAN_SIZE many
times! */
constexpr uint32_t RECV_PARSING_BUF_SIZE = 2 * 1024 * 1024;

/** Size of block reads when the log groups are scanned forward to do a
roll-forward */
#define RECV_SCAN_SIZE (4 * UNIV_PAGE_SIZE)

extern size_t recv_n_frames_for_pages_per_pool_instance;

/** A list of tablespaces for which (un)encryption process was not
completed before crash. */
extern std::list<space_id_t> recv_encr_ts_list;

/** Check the 4-byte checksum to the trailer checksum field of a log
block.
@param[in]  block pointer to a log block
@return whether the checksum matches */
bool log_block_checksum_is_ok(const byte *block);

/** Checks if a given log data block could be considered a next valid block,
with regards to the epoch_no it has stored in its header, during the recovery.
@param[in]  log_block_epoch_no  epoch_no of the log data block to check
@param[in]  last_epoch_no       epoch_no of the last data block scanned
@return true iff the provided log block has valid epoch_no */
bool log_block_epoch_no_is_valid(uint32_t log_block_epoch_no,
                                 uint32_t last_epoch_no);
/** Describes location of a single checkpoint. */
struct Log_checkpoint_location {
  /** File containing checkpoint header and checkpoint lsn. */
  Log_file_id m_checkpoint_file_id{0};

  /** Checkpoint header number. */
  Log_checkpoint_header_no m_checkpoint_header_no{};

  /** Checkpoint LSN. */
  lsn_t m_checkpoint_lsn{0};
};
/** Find the latest checkpoint (check all existing redo log files).
@param[in,out]  log             redo log
@param[out]     checkpoint      the latest checkpoint found (if any)
@return true iff any checkpoint has been found */
bool recv_find_max_checkpoint(log_t &log, Log_checkpoint_location &checkpoint);

/** Reads a specified log segment to a buffer.
@param[in,out]  log   redo log
@param[in,out]  buf   buffer where to read
@param[in]  start_lsn read area start
@param[in]  end_lsn   read area end */
lsn_t recv_read_log_seg(log_t &log, byte *buf, lsn_t start_lsn,
                        const lsn_t end_lsn);

/** Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys->parse_start_lsn is non-zero.
@param[in]  log_block   log block
@param[in]  scanned_lsn  lsn of how far we were able
                         to find data in this log block
@param[in]  len          0 if full block or length of the data to add
@return true if more data added */
bool recv_sys_add_to_parsing_buf(const byte *log_block, lsn_t scanned_lsn,
                                 ulint len);

/** Moves the parsing buffer data left to the buffer start. */
void recv_reset_buffer();

/** Resize the recovery parsing buffer upto log_buffer_size */
bool recv_sys_resize_buf();

/** Parse log records from a buffer and optionally store them to a
hash table to wait merging to file pages. */
void recv_parse_log_recs();

std::tuple<bool, recv_sys_t::Encryption_Key *> recv_find_encryption_key(
    space_id_t space_id);

#include "log0recv.ic"

#endif
