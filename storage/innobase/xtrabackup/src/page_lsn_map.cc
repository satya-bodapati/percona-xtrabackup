/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Page LSN map. See page_lsn_map.h for the format.

The errors here are asymmetric, and every decision follows from that:

  entry missing        -> page is read exactly as today          SAFE
  recorded LSN too low -> redundant records applied, then
                          discarded by the LSN check inside apply SAFE
  recorded LSN too HIGH -> a needed record is skipped, leaving a
                          stale page that still passes its own
                          checksum                                SILENT
CORRUPTION

So: over-include wherever there is any doubt, and drop the whole map rather
than guess.

*******************************************************/

#include "page_lsn_map.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>

#include "common.h"
#include "datasink.h"
#include "fil0fil.h"
#include "fsp0types.h"
#include "mach0data.h"
#include "ut0crc32.h"

/* xtrabackup.cc */
extern char *opt_mysql_tmpdir;

namespace page_lsn_map {

namespace {

/* ------------------------------------------------------------------ *
 * Varint
 *
 * A self-contained LEB128 with an explicit end pointer. Deliberately NOT
 * mach_u64_write_much_compressed(): that routine dispatches on
 * xb_log_detected_format between a v3 and a v4 encoder, so the layout of a
 * file we own would depend on which redo format the source server happened
 * to use. Its read side has no bounds-checked u64 parser in 8.4 either --
 * mach_parse_u64_much_compressed does not exist, only an unchecked
 * mach_read_next_much_compressed_v4 that will happily run off the end of a
 * truncated body. Fifteen lines here buy a parser that cannot.
 * ------------------------------------------------------------------ */

uint32_t varint_size(uint64_t v) {
  uint32_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

byte *varint_write(byte *p, uint64_t v) {
  while (v >= 0x80) {
    *p++ = (byte)(v | 0x80);
    v >>= 7;
  }
  *p++ = (byte)v;
  return p;
}

/** @return false on a truncated or over-long encoding */
bool varint_read(const byte **p, const byte *end, uint64_t *out) {
  uint64_t v = 0;
  unsigned shift = 0;
  while (*p < end) {
    const byte b = **p;
    ++(*p);
    v |= (uint64_t)(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *out = v;
      return true;
    }
    shift += 7;
    if (shift > 63) return false; /* malformed */
  }
  return false; /* truncated */
}

inline bool bit_set(const byte *bm, uint32_t i) {
  return (bm[i >> 3] & (1U << (i & 7))) != 0;
}

inline void set_bit(byte *bm, uint32_t i) { bm[i >> 3] |= (1U << (i & 7)); }

/** Parse and validate a block header. @return false if it is not a block. */
bool parse_header(const byte *h, block_header_t *out) {
  if (mach_read_from_4(h + OFF_MAGIC) != BLOCK_MAGIC) return false;
  if (h[OFF_FORMAT_VERSION] != FORMAT_VERSION) return false;
  out->flags = h[OFF_FLAGS];
  out->generation = mach_read_from_2(h + OFF_GENERATION);
  out->space_id = mach_read_from_4(h + OFF_SPACE_ID);
  out->first_page = mach_read_from_4(h + OFF_FIRST_PAGE);
  out->base_lsn = mach_read_from_8(h + OFF_BASE_LSN);
  out->byte_length = mach_read_from_4(h + OFF_BYTE_LENGTH);
  out->n_entries = mach_read_from_4(h + OFF_N_ENTRIES);
  if (out->n_entries == 0 || out->n_entries > MAX_ENTRIES_PER_BLOCK) {
    return false;
  }
  /* body must at least hold the bitmap */
  if (out->byte_length < (out->n_entries + 7) / 8) return false;
  return true;
}

bool write_all(int fd, const byte *p, size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, p, n);
    if (w <= 0) return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}

/* ------------------------------------------------------------------ *
 * Global backup-side state
 * ------------------------------------------------------------------ */

bool g_capture = false;
lsn_t g_base_lsn = 0;
std::vector<Writer *> g_writers;

/* ------------------------------------------------------------------ *
 * Global prepare-side state
 * ------------------------------------------------------------------ */

struct space_entries_t {
  /* sorted by page_no, keep-first on duplicates */
  std::vector<std::pair<uint32_t, uint64_t>> v;
};

std::unordered_map<uint32_t, space_entries_t> g_map;
bool g_loaded = false;
uint64_t g_n_entries = 0;

/* One-entry memo. Redo records arrive strongly clustered by space, so this
removes the hash lookup for nearly every call. */
thread_local uint32_t t_memo_space = UINT32_MAX;
thread_local const space_entries_t *t_memo = nullptr;

}  // namespace

/* ================================================================== *
 * Writer
 * ================================================================== */

Writer::~Writer() {
  if (m_fd >= 0) ::close(m_fd);
}

bool Writer::open(uint32_t thread_n, uint64_t base_lsn) {
  char buf[FN_REFLEN];
  snprintf(buf, sizeof(buf), "%s/xb_page_lsn_%u.tmp",
           opt_mysql_tmpdir ? opt_mysql_tmpdir : "/tmp", thread_n);
  m_path = buf;
  m_base_lsn = base_lsn;
  m_fd = ::open(m_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (m_fd < 0) {
    xb::warn() << "page LSN map: cannot create " << m_path
               << ", capture disabled for this thread";
    return false;
  }
  return true;
}

bool Writer::flush_block() {
  if (!m_open || m_lsns.empty()) {
    m_open = false;
    m_lsns.clear();
    return true;
  }

  const uint32_t n = (uint32_t)m_lsns.size();
  const uint32_t bm_bytes = (n + 7) / 8;

  /* size the body */
  uint32_t vals_bytes = 0;
  for (uint64_t l : m_lsns) {
    if (l != UINT64_MAX && l > m_base_lsn) {
      vals_bytes += varint_size((uint64_t)(l - m_base_lsn));
    }
  }

  std::vector<byte> blk(BLOCK_HEADER_SIZE + bm_bytes + vals_bytes, 0);
  byte *h = blk.data();
  byte *body = h + BLOCK_HEADER_SIZE;
  byte *vals = body + bm_bytes;

  for (uint32_t i = 0; i < n; i++) {
    const uint64_t l = m_lsns[i];
    if (l != UINT64_MAX && l > m_base_lsn) {
      set_bit(body, i);
      vals = varint_write(vals, (uint64_t)(l - m_base_lsn));
    }
    /* A clear bit means "at or below base_lsn": every record applies. A
    page we never saw (a gap from a read filter) is also left clear, which
    is the safe direction -- it can only cause a page to be read. */
  }
  ut_a(vals == blk.data() + blk.size());

  mach_write_to_4(h + OFF_MAGIC, BLOCK_MAGIC);
  h[OFF_FORMAT_VERSION] = FORMAT_VERSION;
  h[OFF_FLAGS] = 0;
  mach_write_to_2(h + OFF_GENERATION, 0);
  mach_write_to_4(h + OFF_SPACE_ID, m_space_id);
  mach_write_to_4(h + OFF_FIRST_PAGE, m_first_page);
  mach_write_to_8(h + OFF_BASE_LSN, m_base_lsn);
  mach_write_to_4(h + OFF_BYTE_LENGTH, bm_bytes + vals_bytes);
  mach_write_to_4(h + OFF_N_ENTRIES, n);
  mach_write_to_4(h + OFF_CRC32C, 0);
  const uint32_t crc = ut_crc32(h, blk.size());
  mach_write_to_4(h + OFF_CRC32C, crc);

  const bool ok = write_all(m_fd, blk.data(), blk.size());
  m_open = false;
  m_lsns.clear();
  return ok;
}

void Writer::add(uint32_t space_id, uint32_t page_no, uint64_t page_lsn) {
  if (m_fd < 0) return;

  if (m_open && (space_id != m_space_id ||
                 page_no != m_first_page + (uint32_t)m_lsns.size() ||
                 m_lsns.size() >= MAX_ENTRIES_PER_BLOCK)) {
    flush_block();
  }

  if (!m_open) {
    m_open = true;
    m_space_id = space_id;
    m_first_page = page_no;
    m_lsns.clear();
  }

  m_lsns.push_back(page_lsn);
}

bool Writer::close() {
  if (m_fd < 0) return false;
  const bool ok = flush_block();
  ::close(m_fd);
  m_fd = -1;
  return ok;
}

/* ================================================================== *
 * Backup side
 * ================================================================== */

bool capture_enabled() { return g_capture; }

void backup_init(uint64_t base_lsn, uint32_t n_threads) {
  g_base_lsn = base_lsn;
  /* The copy threads number themselves 1..N (xtrabackup.cc sets
  data_threads[i].num = i + 1), so an array of exactly N slots leaves
  thread N with no writer and slot 0 unused. Every page that thread copied
  was then dropped: with --parallel=8 that silently cost 1/8 of all
  tablespaces, and the one it happened to take out carried 37.5% of the
  apply phase's record examinations. Size for the numbering actually
  used, and fill EVERY slot: sizing the array without extending the loop
  that populates it leaves the new slot null and drops exactly as much as
  before, which is what the first attempt at this fix did (507,079 pages,
  caught only because the drop counter below now exists). */
  g_writers.assign(n_threads + 1, nullptr);
  for (uint32_t i = 0; i < g_writers.size(); i++) {
    Writer *w = new Writer();
    if (w->open(i, base_lsn)) {
      g_writers[i] = w;
    } else {
      delete w;
    }
  }
  g_capture = true;
  xb::info() << "page LSN map: capturing, base LSN " << base_lsn;
}

std::atomic<uint64_t> g_dropped_pages{0};

Writer *writer_for_thread(uint32_t thread_n) {
  if (!g_capture) return nullptr;
  if (thread_n >= g_writers.size()) {
    /* Must never happen now that the array is sized for the 1-based
    numbering, but a silent drop here is exactly how the off-by-one above
    survived: the map simply came out short and nothing said so. */
    g_dropped_pages.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return g_writers[thread_n];
}

void capture_buffer(uint32_t space_id, bool is_system, const unsigned char *buf,
                    uint32_t first_page_no, uint32_t n_pages,
                    uint32_t page_size, uint32_t thread_n) {
  Writer *w = writer_for_thread(thread_n);
  if (w == nullptr) {
    g_dropped_pages.fetch_add(n_pages, std::memory_order_relaxed);
    return;
  }

  for (uint32_t i = 0; i < n_pages; i++) {
    const uint32_t page_no = first_page_no + i;

    /* The legacy doublewrite buffer lives in extents 1 and 2 of the system
    tablespace. xb_fil_cur_read_from_offset() deliberately does NOT
    checksum those pages but still counts them into buf_npages -- they are
    copies of OTHER pages and carry those pages' LSNs. Recording one would
    be an overstated LSN for this page number, which is the single
    silent-corruption path in this design. */
    if (is_system && page_no >= FSP_EXTENT_SIZE &&
        page_no < FSP_EXTENT_SIZE * 3) {
      continue;
    }

    const unsigned char *page = buf + (size_t)i * page_size;
    /* FIL_PAGE_LSN is at offset 16, inside the 38-byte header that
    Encryption::encrypt_low() copies in the clear, so this is valid for
    encrypted and page-compressed tablespaces without decrypting. */
    const uint64_t page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);

    w->add(space_id, page_no, page_lsn);
  }
}

void backup_finish_emit(ds_ctxt *ds) {
  if (!g_capture) return;

  /* A map that is merely SHORT still prepares correctly -- a missing entry
  just means the page is read as it always was -- so an incomplete map has
  no symptom other than lost speed. Say so loudly rather than shipping a
  quietly truncated map. */
  const uint64_t dropped = g_dropped_pages.load(std::memory_order_relaxed);
  if (dropped != 0) {
    xb::warn() << "page LSN map: " << dropped
               << " pages were not captured because no writer was available "
                  "for the copy thread; the map will be incomplete and "
                  "prepare will simply be slower for those pages";
  }

  for (Writer *w : g_writers) {
    if (w != nullptr) w->close();
  }

  MY_STAT stat_info;
  memset(&stat_info, 0, sizeof(stat_info));
  stat_info.st_mtime = time(nullptr);

  ds_file_t *out = ds_open(ds, MAP_FILE_NAME, &stat_info);
  if (out == nullptr) {
    xb::warn() << "page LSN map: cannot open " << MAP_FILE_NAME
               << " in the backup; shipping no map";
    backup_cleanup();
    return;
  }

  uint64_t n_blocks = 0, n_entries_total = 0;
  uint32_t crc_of_crcs = 0;
  bool ok = true;

  /* Stream every temp file's blocks into the map file. Blocks are copied
  byte for byte -- the temp format IS the map format, so there is one
  reader for both and nothing is re-encoded here. */
  for (Writer *w : g_writers) {
    if (w == nullptr) continue;
    const int fd = ::open(w->path().c_str(), O_RDONLY);
    if (fd < 0) continue;

    byte hdr[BLOCK_HEADER_SIZE];
    while (true) {
      const ssize_t r = ::read(fd, hdr, BLOCK_HEADER_SIZE);
      if (r == 0) break;
      if (r != (ssize_t)BLOCK_HEADER_SIZE) {
        xb::warn() << "page LSN map: short read in " << w->path()
                   << ", truncating map here";
        break;
      }
      block_header_t h;
      if (!parse_header(hdr, &h)) {
        xb::warn() << "page LSN map: bad block header in " << w->path()
                   << ", truncating map here";
        break;
      }
      std::vector<byte> body(h.byte_length);
      if (::read(fd, body.data(), h.byte_length) != (ssize_t)h.byte_length) {
        xb::warn() << "page LSN map: short body in " << w->path()
                   << ", truncating map here";
        break;
      }
      if (ds_write(out, hdr, BLOCK_HEADER_SIZE) ||
          ds_write(out, body.data(), h.byte_length)) {
        ok = false;
        break;
      }
      ++n_blocks;
      n_entries_total += h.n_entries;
      crc_of_crcs ^= mach_read_from_4(hdr + OFF_CRC32C);
    }
    ::close(fd);
    if (!ok) break;
  }

  if (ok) {
    /* Trailer: prepare validates this before trusting anything. */
    byte tr[32];
    memset(tr, 0, sizeof(tr));
    mach_write_to_4(tr + 0, TRAILER_MAGIC);
    tr[4] = FORMAT_VERSION;
    mach_write_to_8(tr + 8, n_blocks);
    mach_write_to_8(tr + 16, n_entries_total);
    mach_write_to_4(tr + 24, crc_of_crcs);
    mach_write_to_4(tr + 28, ut_crc32(tr, 28));
    if (ds_write(out, tr, sizeof(tr))) ok = false;
  }

  ds_close(out);

  if (ok) {
    xb::info() << "page LSN map: wrote " << MAP_FILE_NAME << ", " << n_blocks
               << " blocks, " << n_entries_total << " page entries";
  } else {
    xb::warn() << "page LSN map: write failed; the map may be unusable. "
                  "Prepare will fall back to reading every page.";
  }

  backup_cleanup();
}

void backup_cleanup() {
  for (Writer *w : g_writers) {
    if (w == nullptr) continue;
    unlink(w->path().c_str());
    delete w;
  }
  g_writers.clear();
  g_capture = false;
}

/* ================================================================== *
 * Prepare side
 * ================================================================== */

bool load(const char *dir) {
  char path[FN_REFLEN];
  snprintf(path, sizeof(path), "%s/%s", dir, MAP_FILE_NAME);

  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) return false; /* no map: prepare as today */

  const off_t size = lseek(fd, 0, SEEK_END);
  if (size < 32) {
    ::close(fd);
    return false;
  }

  /* Validate the trailer before trusting a single block. */
  byte tr[32];
  if (pread(fd, tr, 32, size - 32) != 32 ||
      mach_read_from_4(tr + 0) != TRAILER_MAGIC || tr[4] != FORMAT_VERSION ||
      mach_read_from_4(tr + 28) != ut_crc32(tr, 28)) {
    xb::warn() << "page LSN map: bad or missing trailer in " << path
               << "; ignoring the map and preparing the usual way";
    ::close(fd);
    return false;
  }
  const uint64_t want_entries = mach_read_from_8(tr + 16);

  lseek(fd, 0, SEEK_SET);
  off_t pos = 0;
  const off_t blocks_end = size - 32;
  uint64_t bad_blocks = 0;

  byte hdr[BLOCK_HEADER_SIZE];
  while (pos + (off_t)BLOCK_HEADER_SIZE <= blocks_end) {
    if (pread(fd, hdr, BLOCK_HEADER_SIZE, pos) != (ssize_t)BLOCK_HEADER_SIZE) {
      break;
    }
    block_header_t h;
    if (!parse_header(hdr, &h)) {
      ++bad_blocks;
      break; /* cannot know where the next block starts */
    }
    if (pos + (off_t)BLOCK_HEADER_SIZE + h.byte_length > blocks_end) break;

    std::vector<byte> blk(BLOCK_HEADER_SIZE + h.byte_length);
    if (pread(fd, blk.data(), blk.size(), pos) != (ssize_t)blk.size()) break;

    /* CRC: a block that fails contributes NO entries. Its pages are then
    simply read the old way. */
    const uint32_t stored = mach_read_from_4(blk.data() + OFF_CRC32C);
    mach_write_to_4(blk.data() + OFF_CRC32C, 0);
    const uint32_t actual = ut_crc32(blk.data(), blk.size());
    if (actual != stored) {
      ++bad_blocks;
      pos += BLOCK_HEADER_SIZE + h.byte_length;
      continue;
    }

    const byte *body = blk.data() + BLOCK_HEADER_SIZE;
    const uint32_t bm_bytes = (h.n_entries + 7) / 8;
    const byte *v = body + bm_bytes;
    const byte *end = body + h.byte_length;

    auto &se = g_map[h.space_id];
    se.v.reserve(se.v.size() + h.n_entries);

    bool body_ok = true;
    for (uint32_t i = 0; i < h.n_entries; i++) {
      uint64_t page_lsn;
      if (!bit_set(body, i)) {
        page_lsn = h.base_lsn; /* the floor: every record applies */
      } else {
        uint64_t delta;
        if (!varint_read(&v, end, &delta)) {
          body_ok = false;
          break;
        }
        page_lsn = h.base_lsn + delta;
      }
      se.v.emplace_back(h.first_page + i, page_lsn);
    }

    if (!body_ok || v != end) {
      /* Malformed body. Landing exactly on byte_length is a free integrity
      check; missing it means the block is not what it claims. */
      ++bad_blocks;
      se.v.resize(se.v.size() >= h.n_entries ? se.v.size() - h.n_entries : 0);
    }

    pos += BLOCK_HEADER_SIZE + h.byte_length;
  }

  ::close(fd);

  /* Sort each space by page and keep the FIRST entry for a duplicate: a
  page re-read during the backup has the lower LSN on the earlier read, and
  the lower LSN is the safe one. */
  g_n_entries = 0;
  for (auto &kv : g_map) {
    auto &v = kv.second.v;
    std::stable_sort(v.begin(), v.end(),
                     [](const std::pair<uint32_t, uint64_t> &a,
                        const std::pair<uint32_t, uint64_t> &b) {
                       return a.first < b.first;
                     });
    v.erase(std::unique(v.begin(), v.end(),
                        [](const std::pair<uint32_t, uint64_t> &a,
                           const std::pair<uint32_t, uint64_t> &b) {
                          return a.first == b.first;
                        }),
            v.end());
    g_n_entries += v.size();
  }

  if (g_n_entries == 0) {
    g_map.clear();
    return false;
  }

  g_loaded = true;
  xb::info() << "page LSN map: loaded " << g_n_entries << " page entries from "
             << g_map.size() << " tablespaces"
             << (bad_blocks ? " (some blocks were skipped)" : "")
             << "; trailer claimed " << want_entries;
  return true;
}

bool is_loaded() { return g_loaded; }

/* Diagnostic for the records the filter fails to drop: says whether the map
holds nothing at all for this tablespace, or holds it but stops short of this
page. The two have completely different causes and completely different
fixes, and the counters at the apply site cannot tell them apart. */
void probe(uint32_t space_id, uint32_t page_no, bool *space_present,
           uint64_t *space_n, uint32_t *space_max_page) {
  *space_present = false;
  *space_n = 0;
  *space_max_page = 0;
  if (!g_loaded) return;
  auto it = g_map.find(space_id);
  if (it == g_map.end()) return;
  *space_present = true;
  *space_n = it->second.v.size();
  if (!it->second.v.empty()) *space_max_page = it->second.v.back().first;
  (void)page_no;
}

uint64_t n_entries() { return g_n_entries; }

uint64_t lookup(uint32_t space_id, uint32_t page_no) {
  if (!g_loaded) return 0;

  const space_entries_t *se;
  if (space_id == t_memo_space && t_memo != nullptr) {
    se = t_memo;
  } else {
    auto it = g_map.find(space_id);
    if (it == g_map.end()) {
      t_memo_space = space_id;
      t_memo = nullptr;
      return 0;
    }
    se = &it->second;
    t_memo_space = space_id;
    t_memo = se;
  }
  if (se == nullptr) return 0;

  const auto &v = se->v;
  auto it = std::lower_bound(v.begin(), v.end(), page_no,
                             [](const std::pair<uint32_t, uint64_t> &a,
                                uint32_t p) { return a.first < p; });
  if (it == v.end() || it->first != page_no) return 0;
  return it->second;
}

void unload() {
  g_map.clear();
  g_loaded = false;
  g_n_entries = 0;
  t_memo_space = UINT32_MAX;
  t_memo = nullptr;
}

}  // namespace page_lsn_map
