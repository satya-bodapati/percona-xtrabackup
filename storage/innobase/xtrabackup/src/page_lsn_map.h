/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Page LSN map: record every copied page's FIL_PAGE_LSN during --backup so that
--prepare can decide whether a redo record is already contained in a page
without reading the page.

*******************************************************/

#ifndef XB_PAGE_LSN_MAP_H
#define XB_PAGE_LSN_MAP_H

#include <cstdint>
#include <string>
#include <vector>

struct ds_ctxt;

namespace page_lsn_map {

/** Name of the map file inside the backup. */
constexpr const char *MAP_FILE_NAME = "xtrabackup_page_lsn";

/* ------------------------------------------------------------------ *
 * On-disk block format
 *
 * A block covers a contiguous run of pages of ONE tablespace, so page
 * numbers are implied by position and cost nothing. The body is a presence
 * bitmap of n_entries bits followed by one compressed integer per SET bit,
 * in bitmap order, each the page's LSN minus base_lsn.
 *
 * A CLEAR bit is not missing data: it means the page's LSN is at or below
 * base_lsn (the backup's start checkpoint), so every redo record in the
 * backup applies to it and the exact value is never needed. On a typical
 * instance most pages sit at that floor and cost one bit each.
 *
 * The same format is used for the per-thread temp files and for the map
 * file that ships in the backup, so there is one reader, not two.
 * ------------------------------------------------------------------ */

/** "XBPL" */
constexpr uint32_t BLOCK_MAGIC = 0x5842504CU;
/** "XBPT", the file trailer */
constexpr uint32_t TRAILER_MAGIC = 0x58425054U;
constexpr uint8_t FORMAT_VERSION = 1;

/** Header size. 34 bytes of fields plus 6 reserved, rounded to 40 so a
format v2 can add a field without changing the block size. */
constexpr uint32_t BLOCK_HEADER_SIZE = 40;

/* Field offsets within the header. */
constexpr uint32_t OFF_MAGIC = 0;
constexpr uint32_t OFF_FORMAT_VERSION = 4;
constexpr uint32_t OFF_FLAGS = 5;
constexpr uint32_t OFF_GENERATION = 6;
constexpr uint32_t OFF_SPACE_ID = 8;
constexpr uint32_t OFF_FIRST_PAGE = 12;
constexpr uint32_t OFF_BASE_LSN = 16;
constexpr uint32_t OFF_BYTE_LENGTH = 24;
constexpr uint32_t OFF_N_ENTRIES = 28;
constexpr uint32_t OFF_CRC32C = 32;

/** Entries per block. A block is sealed when it fills or when the copy
thread moves to another tablespace. */
constexpr uint32_t MAX_ENTRIES_PER_BLOCK = 16384;

/** Parsed block header. */
struct block_header_t {
  uint32_t space_id;
  uint32_t first_page;
  uint32_t n_entries;
  uint32_t byte_length;
  uint64_t base_lsn;
  uint16_t generation;
  uint8_t flags;
};

/* ------------------------------------------------------------------ *
 * Backup side
 * ------------------------------------------------------------------ */

/** Per-copy-thread writer. Append only, never fsynced; the file is deleted
when the backup ends. One instance per data copy thread, so no locking. */
class Writer {
 public:
  Writer() = default;
  ~Writer();

  /** Open the temp file for thread `thread_n`. */
  bool open(uint32_t thread_n, uint64_t base_lsn);

  /** Record one page. Pages must arrive in ascending page_no within a
  tablespace; a gap or a space change seals the open block. */
  void add(uint32_t space_id, uint32_t page_no, uint64_t page_lsn);

  /** Seal the open block and append the file trailer. */
  bool close();

  const std::string &path() const { return m_path; }

 private:
  bool flush_block();

  int m_fd{-1};
  std::string m_path;
  uint64_t m_base_lsn{0};

  /* the open block */
  bool m_open{false};
  uint32_t m_space_id{0};
  uint32_t m_first_page{0};
  std::vector<uint64_t> m_lsns; /* UINT64_MAX means "no entry" */

  /* trailer accumulation: space -> max generation seen */
  std::vector<std::pair<uint32_t, uint16_t>> m_spaces;
};

/** True when --page-lsn-map was requested and the backup is eligible. */
bool capture_enabled();

/** Called once from xtrabackup_backup_func() before the copy threads start. */
void backup_init(uint64_t base_lsn, uint32_t n_threads);

/** The writer belonging to a data copy thread, or nullptr when capture is
off. */
Writer *writer_for_thread(uint32_t thread_n);

/** Record every page of a cursor buffer that has just been read and
verified. Skips the legacy doublewrite range of the system tablespace,
whose pages carry OTHER pages' LSNs. */
void capture_buffer(uint32_t space_id, bool is_system, const unsigned char *buf,
                    uint32_t first_page_no, uint32_t n_pages,
                    uint32_t page_size, uint32_t thread_n);

/** Close all writers, filter the temp files and emit the map into the
backup through `ds`. Called from backup_finish() after unlock_all().
Never fatal: on any problem it logs and ships no map, which simply means
prepare reads pages the way it does today. */
void backup_finish_emit(ds_ctxt *ds);

/** Delete the temp files. */
void backup_cleanup();

/* ------------------------------------------------------------------ *
 * Prepare side
 * ------------------------------------------------------------------ */

/** Load the map from `dir`. Returns false when there is no usable map, in
which case prepare behaves exactly as it does today. */
bool load(const char *dir);

/** Is a map loaded and in use? */
bool is_loaded();

/** LSN the page had when it was copied into the backup, or 0 when the map
holds no entry for it. */
uint64_t lookup(uint32_t space_id, uint32_t page_no);

/** Number of entries loaded, for the prepare stats line. */
uint64_t n_entries();

void unload();

}  // namespace page_lsn_map

#endif /* XB_PAGE_LSN_MAP_H */
