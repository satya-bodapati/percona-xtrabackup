/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Multipart upload orchestration for xbcloud (PXB-3671 prototype).

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*******************************************************/

#include "multipart.h"

#include <algorithm>

#include "msg.h"
#include "my_sys.h"

namespace xbcloud {

namespace stats {
std::atomic<uint64_t> total_bytes_appended{0};
std::atomic<uint64_t> total_bytes_uploaded{0};
std::atomic<int> total_parts_inflight{0};
std::atomic<int> total_files_inflight{0};
}  // namespace stats

namespace {
constexpr uint64_t MIB = 1024ULL * 1024ULL;
constexpr uint64_t GIB = 1024ULL * MIB;
constexpr uint64_t TIB = 1024ULL * GIB;
}  // namespace

std::string build_rollover_manifest(
    const std::string &logical_name, uint64_t total_size,
    uint64_t rollover_threshold,
    const std::vector<rollover_segment_t> &segments) {
  /* Hand-built JSON to avoid pulling in another dependency for a tiny
     fixed-shape document. Keys are known-safe ASCII identifiers; only
     logical_name needs string-escaping (limited to escaping \ and "). */
  auto escape = [](const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
      if (c == '\\' || c == '"') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  };
  std::string body;
  body.reserve(256 + segments.size() * 96);
  body += "{\n";
  body += "  \"logical_name\": \"" + escape(logical_name) + "\",\n";
  body += "  \"total_size\": " + std::to_string(total_size) + ",\n";
  body += "  \"rollover_threshold\": " +
          std::to_string(rollover_threshold) + ",\n";
  body += "  \"segments\": [\n";
  for (size_t i = 0; i < segments.size(); ++i) {
    body += "    {\"key\": \"" + escape(segments[i].key) +
            "\", \"size\": " + std::to_string(segments[i].size) + "}";
    if (i + 1 < segments.size()) body += ",";
    body += "\n";
  }
  body += "  ]\n";
  body += "}\n";
  return body;
}

size_t dynamic_part_size(uint64_t bytes_so_far) {
  /* Tiered ramp; see multipart.h for the rationale and parts-count
     accounting. Both the streaming path and the known-size path call
     this with their respective "bytes so far" value (running total for
     streaming, stat'd file_size for known). */
  if (bytes_so_far < 1ULL * GIB) return 16ULL * MIB;
  if (bytes_so_far < 10ULL * GIB) return 64ULL * MIB;
  if (bytes_so_far < 100ULL * GIB) return 256ULL * MIB;
  if (bytes_so_far < 1ULL * TIB) return 512ULL * MIB;
  return 600ULL * MIB;
}

Multipart_uploader::~Multipart_uploader() {
  if (m_started && !m_failed.load()) {
    msg_ts(
        "%s: Multipart_uploader destroyed without commit/abort; cancelling "
        "%s upload_id=%s\n",
        my_progname, m_helper->object_name().c_str(), m_upload_id.c_str());
    abort();
  }
}

bool Multipart_uploader::start() {
  if (m_started) return true;
  if (!m_helper->init(m_upload_id)) {
    m_failed = true;
    return false;
  }
  m_started = true;
  stats::total_files_inflight.fetch_add(1, std::memory_order_relaxed);
  msg_ts("%s: multipart start %s upload_id=%s\n", my_progname,
         m_helper->object_name().c_str(), m_upload_id.c_str());
  return true;
}

void Multipart_uploader::on_part_complete(int part_number, size_t bytes,
                                          bool ok, std::string part_id) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ok) {
      m_parts.emplace_back(part_number, std::move(part_id));
      m_bytes_uploaded.fetch_add(bytes, std::memory_order_relaxed);
      stats::total_bytes_uploaded.fetch_add(bytes, std::memory_order_relaxed);
    } else {
      m_failed = true;
    }
    if (m_inflight_bytes >= bytes) {
      m_inflight_bytes -= bytes;
    } else {
      m_inflight_bytes = 0;
    }
    m_parts_inflight.fetch_sub(1, std::memory_order_relaxed);
    stats::total_parts_inflight.fetch_sub(1, std::memory_order_relaxed);
  }
  m_cv.notify_all();
  if (ok) {
    msg_ts("%s: multipart %s part #%d done (%zu bytes)\n", my_progname,
           m_helper->object_name().c_str(), part_number, bytes);
  } else {
    msg_ts("%s: multipart %s part #%d FAILED (%zu bytes)\n", my_progname,
           m_helper->object_name().c_str(), part_number, bytes);
  }
}

bool Multipart_uploader::upload_part(int part_number, const char *data,
                                     size_t size) {
  /* Lazy Init: only call InitiateMultipartUpload when bytes actually
     need to be pushed. Verified via perf_wan.sh: eager Init at first
     chunk of every file caused multipart mode to spend 8.4 s in sync
     POST/DELETE round-trips on a workload that legacy did in 0.4 s.

     With lazy Init: small files take the EOF small-file fast path
     (single async PUT) and never call Init/Abort. Large files call
     Init exactly once, when the first part is ready to flush, so the
     Init RTT overlaps the buffering of subsequent bytes. */
  if (!m_started) {
    if (!start()) return false;
  }
  if (m_failed.load()) return false;

  /* Backpressure: block here when admitting this part would push the
     total in-flight buffer over memory_budget. Always admit when nothing
     is in flight, so a single oversized part (e.g. last part of a known
     file that exceeds the budget) is not deadlocked. The cv is notified
     by on_part_complete (bytes freed) or by abort (failure). */
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this, size] {
      if (m_failed.load()) return true;
      if (m_inflight_bytes == 0) return true;
      return m_inflight_bytes + size <= m_memory_budget;
    });
    if (m_failed.load()) return false;
    m_inflight_bytes += size;
    m_parts_inflight.fetch_add(1, std::memory_order_relaxed);
    stats::total_parts_inflight.fetch_add(1, std::memory_order_relaxed);
  }

  m_bytes_appended.fetch_add(size, std::memory_order_relaxed);
  stats::total_bytes_appended.fetch_add(size, std::memory_order_relaxed);

  bool submitted = m_helper->upload_part_async(
      m_upload_id, part_number, data, size, m_event_handler,
      [this, part_number, size](bool ok, std::string part_id) {
        on_part_complete(part_number, size, ok, std::move(part_id));
      });

  if (!submitted) {
    /* Submission itself failed (e.g. out of memory). The callback will
       NOT fire, so we have to clean up the counter ourselves. */
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_inflight_bytes >= size) {
        m_inflight_bytes -= size;
      } else {
        m_inflight_bytes = 0;
      }
      m_parts_inflight.fetch_sub(1, std::memory_order_relaxed);
      stats::total_parts_inflight.fetch_sub(1, std::memory_order_relaxed);
      m_failed = true;
    }
    m_cv.notify_all();
    return false;
  }
  return true;
}

bool Multipart_uploader::commit() {
  if (!m_started) return false;

  /* Wait for all in-flight callbacks to fire. */
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_inflight_bytes == 0; });
  }

  if (m_failed.load()) {
    abort();
    return false;
  }

  /* Parts may have completed out of order. Sort by part_number before
     handing the list to complete(). */
  std::sort(m_parts.begin(), m_parts.end(),
            [](const multipart_part_t &a, const multipart_part_t &b) {
              return a.first < b.first;
            });

  if (!m_helper->complete(m_upload_id, m_parts)) {
    m_failed = true;
    abort();
    return false;
  }

  /* Mark as no longer active; the destructor will not try to abort. */
  m_started = false;
  stats::total_files_inflight.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

void Multipart_uploader::abort() {
  if (!m_started) return;
  m_failed = true;
  m_cv.notify_all();
  /* Wait for any in-flight callbacks to settle so we do not free state
     while a callback might still touch it. We cannot cancel an in-flight
     curl request mid-flight, so we wait for it to complete (or fail).
     Failed parts decrement inflight_bytes without recording themselves. */
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_inflight_bytes == 0; });
  }
  m_helper->abort(m_upload_id);
  m_started = false;
  stats::total_files_inflight.fetch_sub(1, std::memory_order_relaxed);
}

/* -------------------------------------------------------------------
   Stream_multipart_writer

   Shared driver loop that both xbcloud's put_func and xtrabackup's
   ds_cloud use to ingest bytes into a Multipart_uploader. Consolidates
   the dynamic_part_size flush loop + small-file fast path so a single
   piece of code is the source of truth for "what does multipart-upload-
   of-one-streaming-file" look like.
   -------------------------------------------------------------------*/

bool Stream_multipart_writer::flush_full_parts() {
  while (!m_uploader.failed()) {
    size_t part_size =
        m_fixed_part_size_override != 0
            ? m_fixed_part_size_override
            : dynamic_part_size(m_uploader.bytes_appended());
    if (m_part_buf.size() < part_size) break;
    if (!m_uploader.upload_part(m_next_part_num, m_part_buf.begin(),
                                 part_size)) {
      msg_ts(
          "%s: Stream_multipart_writer: upload_part #%d failed for %s\n",
          my_progname, m_next_part_num, m_object.c_str());
      return false;
    }
    Http_buffer leftover;
    if (m_part_buf.size() > part_size) {
      leftover.append(m_part_buf.begin() + part_size,
                      m_part_buf.size() - part_size);
    }
    m_part_buf = std::move(leftover);
    ++m_next_part_num;
  }
  return !m_uploader.failed();
}

bool Stream_multipart_writer::append(const char *data, size_t size) {
  if (m_closed) {
    msg_ts("%s: Stream_multipart_writer: append after close on %s\n",
           my_progname, m_object.c_str());
    return false;
  }
  if (size > 0) {
    m_part_buf.append(data, size);
  }
  return flush_full_parts();
}

bool Stream_multipart_writer::close() {
  if (m_closed) return true;
  m_closed = true;

  /* Small-file fast path: never opened a multipart session AND the
     entire stream fits below the threshold -> ship as one PUT. Skips
     Initiate + UploadPart + Complete round-trips. */
  bool small_file =
      m_next_part_num == 1 && m_part_buf.size() <= m_small_file_threshold;

  if (small_file) {
    m_small_file_path_taken = true;
    /* Belt-and-suspenders: if a prior failure path started the
       uploader, clean it up. With lazy Init this shouldn't happen. */
    if (m_uploader.started()) m_uploader.abort();

    Http_buffer body;
    if (m_part_buf.size() > 0) {
      body.append(m_part_buf.begin(), m_part_buf.size());
    }
    m_part_buf = Http_buffer{};

    if (m_async_small_file) {
      return m_async_small_file(m_object, body);
    }
    return m_store->upload_object(m_container, m_object, body);
  }

  /* Multipart path: flush remainder (allowed < 5 MiB as last part by
     S3), then commit. */
  if (m_part_buf.size() > 0) {
    if (!m_uploader.upload_part(m_next_part_num, m_part_buf.begin(),
                                 m_part_buf.size())) {
      msg_ts(
          "%s: Stream_multipart_writer: final upload_part #%d failed for %s\n",
          my_progname, m_next_part_num, m_object.c_str());
      return false;
    }
    m_part_buf = Http_buffer{};
    ++m_next_part_num;
  }
  if (!m_uploader.commit()) {
    msg_ts("%s: Stream_multipart_writer: commit failed for %s\n",
           my_progname, m_object.c_str());
    return false;
  }
  return true;
}

void Stream_multipart_writer::abort() {
  if (m_closed) return;
  m_closed = true;
  if (m_uploader.started()) {
    m_uploader.abort();
  }
}

}  // namespace xbcloud
