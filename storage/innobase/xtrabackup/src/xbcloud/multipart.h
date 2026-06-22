/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Multipart upload orchestration for xbcloud (PXB-3671 prototype).

Async multipart uploader built on xbcloud's existing libev + libcurl-multi
Event_handler. A single producer thread can have many parts in flight
concurrently — submission is non-blocking (modulo backpressure when the
in-flight cap is reached). The producer never sees part ordering or
ETags; it just hands bytes to upload_part() with an assigned part_number
and calls commit() when the file is complete. commit() blocks on a
condition variable until every submitted part has completed, then sorts
the part list and calls CompleteMultipartUpload (or backend equivalent).

The interface shape (IMultipart_helper + Multipart_uploader) is inspired
by mysql-shell's mysqlshdk/libs/storage/backend/multipart_upload.{h,cc}.
Unlike mysql-shell which is sync per part, our upload submission is
async via Event_handler so a single thread can saturate the wire.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef XBCLOUD_MULTIPART_H
#define XBCLOUD_MULTIPART_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "http.h"
#include "object_store.h"

namespace xbcloud {

class Event_handler;

/* Process-wide observability counters. Updated by Multipart_uploader on
   every submit / complete / start / commit / abort. Polled by
   Event_handler's rate-logger timer (which runs on the libev thread).
   Atomics so the polling thread sees consistent values without locks. */
namespace stats {
extern std::atomic<uint64_t> total_bytes_appended;
extern std::atomic<uint64_t> total_bytes_uploaded;
extern std::atomic<int> total_parts_inflight;
extern std::atomic<int> total_files_inflight;
}  // namespace stats

/**
 * Per-backend hook for one logical file's multipart upload.
 *
 * init / complete / abort are synchronous and called only by the
 * coordinator thread.
 *
 * upload_part_async submits the HTTP request through Event_handler and
 * returns immediately. The callback fires from the Event_handler thread
 * once the part completes or fails. Bytes are copied into the request
 * body before upload_part_async returns, so the caller may release the
 * `data` buffer immediately.
 */
class IMultipart_helper {
 public:
  using part_callback_t = std::function<void(bool ok, std::string part_id)>;

  virtual ~IMultipart_helper() = default;

  /** Open a server-side multipart session (sync). */
  virtual bool init(std::string &upload_id) = 0;

  /**
   * Submit one part for async upload. Returns true if submission was
   * accepted (does not mean the upload succeeded yet — callback fires
   * later). Returns false if submission itself failed (out of memory,
   * signer error, etc.) and the callback will NOT fire.
   */
  virtual bool upload_part_async(const std::string &upload_id, int part_number,
                                 const char *data, size_t size,
                                 Event_handler *event_handler,
                                 part_callback_t callback) = 0;

  /** Finalize after every submitted part has fired its callback (sync). */
  virtual bool complete(const std::string &upload_id,
                        const std::vector<multipart_part_t> &parts) = 0;

  /** Best-effort cleanup on failure (sync). */
  virtual bool abort(const std::string &upload_id) = 0;

  /** For logging. */
  virtual const std::string &object_name() const = 0;
};

/**
 * Async multipart uploader. One instance per file.
 *
 * Producer thread submits parts via upload_part(); each call is async
 * (returns once the request is in Event_handler's queue). Producer
 * blocks briefly only when the total bytes of in-flight parts would
 * exceed memory_budget; it then waits on a cv for any in-flight part's
 * completion callback to free buffer space.
 *
 * The cap is byte-based rather than count-based because part sizes
 * vary across a single upload — dynamic_part_size() ramps from 16 MiB
 * for the first GiB to 600 MiB above 1 TiB. A fixed count would mean
 * either tiny peak memory (count tuned for big parts) or runaway
 * memory (count tuned for small parts). A byte budget naturally caps
 * RAM and lets the count of in-flight parts scale inversely with part
 * size.
 *
 * commit() waits via cv until inflight_bytes == 0, then sorts the part
 * list by part_number and calls the backend's complete().
 *
 * Threading:
 *   - start / commit / abort: called from the producer thread only.
 *     No concurrent commit/abort.
 *   - upload_part: thread-safe across distinct part_numbers if the
 *     caller wants to dispatch multiple parts from multiple threads
 *     (though the typical model is one producer thread).
 *   - completion callbacks: fire on Event_handler's thread and update
 *     internal state under m_mutex.
 *
 * Observability counters bumped internally:
 *   - bytes_appended on upload_part submission
 *   - bytes_uploaded on successful callback
 *   - parts_inflight up on submit, down on callback
 */
class Multipart_uploader {
 public:
  Multipart_uploader(IMultipart_helper *helper, Event_handler *event_handler,
                     uint64_t memory_budget)
      : m_helper(helper),
        m_event_handler(event_handler),
        m_memory_budget(memory_budget == 0 ? 1 : memory_budget) {}

  Multipart_uploader(const Multipart_uploader &) = delete;
  Multipart_uploader &operator=(const Multipart_uploader &) = delete;

  ~Multipart_uploader();

  /** Sync. Opens the server-side multipart session. */
  bool start();

  /**
   * Submit one part for async upload. May block briefly when in-flight
   * bytes would exceed memory_budget. Returns true if the part was
   * submitted (failure may still arrive via callback later).
   */
  bool upload_part(int part_number, const char *data, size_t size);

  /** Sync. Wait for all in-flight parts, sort, then call complete. */
  bool commit();

  /** Cancel: best-effort abort on the server. */
  void abort();

  bool started() const { return m_started; }
  bool failed() const { return m_failed.load(); }

  /* Observability accessors. Producers and stats loggers can read these
     to diagnose throughput at the producer side (bytes_appended) vs the
     consumer side (bytes_uploaded). */
  uint64_t bytes_appended() const { return m_bytes_appended.load(); }
  uint64_t bytes_uploaded() const { return m_bytes_uploaded.load(); }
  int parts_inflight() const { return m_parts_inflight.load(); }

 private:
  void on_part_complete(int part_number, size_t bytes, bool ok,
                        std::string part_id);

  IMultipart_helper *m_helper;
  Event_handler *m_event_handler;
  uint64_t m_memory_budget;

  std::string m_upload_id;
  bool m_started{false};
  std::atomic<bool> m_failed{false};

  std::mutex m_mutex;
  std::condition_variable m_cv;
  uint64_t m_inflight_bytes{0};
  std::vector<multipart_part_t> m_parts;

  std::atomic<uint64_t> m_bytes_appended{0};
  std::atomic<uint64_t> m_bytes_uploaded{0};
  std::atomic<int> m_parts_inflight{0};
};

/**
 * IMultipart_helper implementation backed by Object_store. Forwards
 * each method to the matching Object_store virtual.
 */
class Object_store_multipart_helper : public IMultipart_helper {
 public:
  Object_store_multipart_helper(Object_store *store, std::string container,
                                std::string object)
      : m_store(store),
        m_container(std::move(container)),
        m_object(std::move(object)) {}

  bool init(std::string &upload_id) override {
    return m_store->init_multipart_upload(m_container, m_object, upload_id);
  }

  bool upload_part_async(const std::string &upload_id, int part_number,
                         const char *data, size_t size,
                         Event_handler *event_handler,
                         part_callback_t callback) override {
    Http_buffer buf;
    if (size > 0) buf.append(data, size);
    return m_store->upload_part_async(m_container, m_object, upload_id,
                                      part_number, buf, event_handler,
                                      std::move(callback));
  }

  bool complete(const std::string &upload_id,
                const std::vector<multipart_part_t> &parts) override {
    return m_store->complete_multipart_upload(m_container, m_object, upload_id,
                                              parts);
  }

  bool abort(const std::string &upload_id) override {
    return m_store->abort_multipart_upload(m_container, m_object, upload_id);
  }

  const std::string &object_name() const override { return m_object; }

 private:
  Object_store *m_store;
  std::string m_container;
  std::string m_object;
};

/**
 * Streaming multipart writer. Owns one Multipart_uploader plus the
 * per-file part_buf and counters. Encapsulates the dynamic_part_size
 * flush loop and the small-file single-PUT fast path so callers don't
 * each open-code it.
 *
 * Used by:
 *   - xbcloud's put_func (one writer per file in the mpfilehash map).
 *   - xtrabackup's ds_cloud (one writer per ds_file_t).
 *   - xbcloud's --multipart-from-file mode (one writer per segment).
 *
 * Threading model: same as Multipart_uploader -- producer thread calls
 * append() and close() serially per writer. Part uploads happen async
 * on Event_handler's libev thread.
 *
 * Lazy Init is inherited from Multipart_uploader: no InitiateMultipart
 * is issued until the first part actually flushes.
 *
 * Small-file fast path: at close(), if no parts have been submitted
 * yet AND the accumulated buffer is <= small_file_threshold, the
 * writer ships the whole buffer as a single PUT (sync by default; an
 * async variant is available via set_async_small_file_uploader, used
 * by xbcloud to keep the producer thread non-blocking when ingesting
 * many tiny files).
 */
class Stream_multipart_writer {
 public:
  /* Caller-supplied function that performs an async single-PUT when the
     writer decides the small-file fast path applies. Receives the
     object key + body; should return true on accepted submission (the
     PUT itself fires async). When unset, close() falls back to a
     synchronous store->upload_object.

     In practice ALL current callers install an async uploader.
     Empirical measurement (perf_wan.sh + real AWS backups): sync
     small-file PUT bottlenecks at WAN RTT regardless of whether the
     caller is single-producer (xbcloud's xbstream-reader) or
     multi-worker (xtrabackup's --parallel=N data-copy threads).

     For multi-worker callers the per-worker serialization is the
     killer -- worker N still walks its assigned files in sequence and
     stalls ~RTT per file on a sync PUT, regardless of what workers
     1..N-1 and N+1..K are doing. At --parallel=4 this halves
     throughput on small-file-heavy backups.

     For continuous-stream callers like ds_redo (xtrabackup's redo log
     reader feeding ds_cloud) blocking the producer is functionally
     unacceptable -- the redo reader must keep consuming for the
     backup to complete.

     The sync default is retained as a fallback for future callers
     that explicitly don't want async (e.g., a small CLI tool with no
     Event_handler scaffolding). The expected pattern is: any caller
     wired to an Event_handler installs the async uploader. */
  using async_small_file_fn =
      std::function<bool(const std::string &object,
                         const Http_buffer &body)>;

  Stream_multipart_writer(Object_store *store, std::string container,
                          std::string object, Event_handler *event_handler,
                          uint64_t memory_budget,
                          size_t small_file_threshold,
                          size_t fixed_part_size_override = 0)
      : m_store(store),
        m_container(std::move(container)),
        m_object(std::move(object)),
        m_helper(m_store, m_container, m_object),
        m_uploader(&m_helper, event_handler, memory_budget),
        m_small_file_threshold(small_file_threshold),
        m_fixed_part_size_override(fixed_part_size_override) {}

  Stream_multipart_writer(const Stream_multipart_writer &) = delete;
  Stream_multipart_writer &operator=(const Stream_multipart_writer &) = delete;

  /* Use an async small-file PUT instead of the default sync upload. */
  void set_async_small_file_uploader(async_small_file_fn fn) {
    m_async_small_file = std::move(fn);
  }

  /* Append bytes; may flush full parts as they accumulate. Returns false
     on failure (upload_part rejected, etc.). */
  bool append(const char *data, size_t size);

  /* EOF: either single-PUT fast path or final-flush + commit. After
     close() the writer is finalized. Returns false on failure. */
  bool close();

  uint64_t bytes_appended() const { return m_uploader.bytes_appended(); }
  uint64_t bytes_uploaded() const { return m_uploader.bytes_uploaded(); }
  bool small_file_path_taken() const { return m_small_file_path_taken; }
  const std::string &object_name() const { return m_object; }

  /* Cancel any in-flight upload. Best effort. */
  void abort();

 private:
  bool flush_full_parts();

  Object_store *m_store;
  std::string m_container;
  std::string m_object;
  Object_store_multipart_helper m_helper;
  Multipart_uploader m_uploader;
  size_t m_small_file_threshold;
  size_t m_fixed_part_size_override;

  Http_buffer m_part_buf;
  int m_next_part_num{1};
  bool m_small_file_path_taken{false};
  bool m_closed{false};
  async_small_file_fn m_async_small_file;
};

/**
 * One segment of a rolled-over file. A logical file larger than
 * --multipart-rollover-threshold is split across several objects in
 * the bucket; the manifest sidecar records the order and per-segment
 * size so the download path can stitch them back together.
 */
struct rollover_segment_t {
  std::string key;  /* object key, e.g. "backup/ibdata1.part-001" */
  uint64_t size;    /* bytes uploaded into this segment */
};

/**
 * Build the manifest JSON written as <object>.manifest.json when a
 * file is rolled over into multiple segments. Format:
 *   {
 *     "logical_name": "ibdata1",
 *     "total_size":   12000000000000,
 *     "rollover_threshold": 4500000000000,
 *     "segments": [
 *       {"key": "backup/ibdata1.part-001", "size": 4500000000000},
 *       {"key": "backup/ibdata1.part-002", "size": 4500000000000},
 *       {"key": "backup/ibdata1.part-003", "size": 3000000000000}
 *     ]
 *   }
 */
std::string build_rollover_manifest(
    const std::string &logical_name, uint64_t total_size,
    uint64_t rollover_threshold,
    const std::vector<rollover_segment_t> &segments);

/**
 * Tiered part-size schedule used by both the streaming put_func path
 * (where bytes_so_far is the running total of bytes already appended
 * to the uploader) and the known-size --multipart-from-file path
 * (where bytes_so_far is the file's stat'd size).
 *
 *   bytes_so_far range   part_size  parts in tier  cumulative parts
 *   ------------------   ---------  -------------  ----------------
 *   < 1 GiB               16 MiB           64                64
 *   1 GiB - 10 GiB        64 MiB          144               208
 *   10 GiB - 100 GiB     256 MiB          360               568
 *   100 GiB - 1 TiB      512 MiB         1848              2416
 *   >= 1 TiB             600 MiB         6826              9242   (at 5 TiB)
 *
 * A 5 TiB total uses ~9242 parts, leaving ~750 parts of headroom
 * against S3's 10000-part hard cap. Below 1 GiB the schedule keeps
 * parts small so tiny streams (e.g. an initial-restore redo log) do
 * not allocate hundreds of MiB of unused buffer. Above 1 TiB the
 * schedule plateaus at 600 MiB per part; combined with the byte-based
 * memory_budget cap in Multipart_uploader, peak RAM per file stays
 * bounded regardless of stream size.
 *
 * Above 5 TiB the design calls for splitting the logical key into
 * NAME.part-001, NAME.part-002, ... (S3 single-object hard cap is
 * 5 TiB). Phase 2 work; see Section 4.4 of the redesign doc.
 */
size_t dynamic_part_size(uint64_t bytes_so_far);

}  // namespace xbcloud

#endif  // XBCLOUD_MULTIPART_H
