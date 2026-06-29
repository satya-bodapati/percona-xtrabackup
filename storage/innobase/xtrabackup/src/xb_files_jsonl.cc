/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

backup_files.jsonl streaming writer.

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

#include "xb_files_jsonl.h"

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "common.h"
#include "msg.h"

namespace {

/* Singleton state held in the .cc to keep the public header
free of rapidjson includes.  Lifetime: from begin() to finalize(). */
struct writer_state_t {
  int fd = -1;
  std::string staging_path;
  std::atomic<size_t> bytes_since_sync{0};
  std::mutex sync_mutex;          /* protects the time-window check */
  std::mutex big_line_mutex;      /* taken only for lines > PIPE_BUF */
  std::chrono::steady_clock::time_point last_sync;
  std::atomic<bool> active{false};
};

writer_state_t g_state;

constexpr size_t kSyncBytesThreshold = 1 * 1024 * 1024;  /* 1 MiB */
constexpr int kSyncSecondsThreshold = 1;
constexpr size_t kPipeBuf = 4096;

/* Allocator + lifetime helper.  Each per-file document owns its own
allocator instance for thread-safe construction. */
struct file_ctx_holder_t {
  rapidjson::Document doc;
  file_ctx_holder_t() : doc(rapidjson::kObjectType) {}
};

inline file_ctx_holder_t *as_holder(void *p) {
  return static_cast<file_ctx_holder_t *>(p);
}

/* Flush the rate-limited fdatasync if either the byte or time
threshold has been crossed.  Cheap fast-path: atomic read +
comparison; mutex only when crossing. */
void maybe_sync(size_t just_appended) {
  const size_t total =
      g_state.bytes_since_sync.fetch_add(just_appended,
                                          std::memory_order_relaxed) +
      just_appended;
  using clk = std::chrono::steady_clock;
  if (total < kSyncBytesThreshold &&
      clk::now() - g_state.last_sync <
          std::chrono::seconds(kSyncSecondsThreshold)) {
    return;
  }

  std::lock_guard<std::mutex> lk(g_state.sync_mutex);
  /* Re-check under the lock; another thread may have synced. */
  if (g_state.bytes_since_sync.load(std::memory_order_relaxed) <
          kSyncBytesThreshold &&
      clk::now() - g_state.last_sync <
          std::chrono::seconds(kSyncSecondsThreshold)) {
    return;
  }
  if (g_state.fd >= 0) {
    fdatasync(g_state.fd);
  }
  g_state.bytes_since_sync.store(0, std::memory_order_relaxed);
  g_state.last_sync = clk::now();
}

}  // namespace

namespace xb_files_jsonl {

bool begin(const char *staging_dir) {
  if (g_state.active.load()) {
    msg("xb_files_jsonl::begin: already active\n");
    return false;
  }
  /* Use a hidden, pid-suffixed staging filename so it does not
  collide with the final XB_BACKUP_FILES_JSONL that publish() writes
  through ds_open_single_object into the same directory in local
  mode, and does not collide with a sibling xtrabackup process that
  happens to default to the same target_dir
  (./xtrabackup_backupfiles/) when --stream is used without
  --target-dir -- e.g. the parallel PXB test harness. */
  char pid_buf[32];
  snprintf(pid_buf, sizeof(pid_buf), ".%ld", (long)getpid());
  g_state.staging_path = staging_dir;
  g_state.staging_path += "/.";
  g_state.staging_path += XB_BACKUP_FILES_JSONL;
  g_state.staging_path += pid_buf;
  g_state.staging_path += ".staging";

  g_state.fd = ::open(g_state.staging_path.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (g_state.fd < 0) {
    msg("xb_files_jsonl::begin: failed to open %s\n",
        g_state.staging_path.c_str());
    return false;
  }

  /* Header line: {"manifest_version": 1}\n */
  const char header[] = "{\"manifest_version\": 1}\n";
  if (::write(g_state.fd, header, sizeof(header) - 1) !=
      (ssize_t)(sizeof(header) - 1)) {
    msg("xb_files_jsonl::begin: header write failed\n");
    ::close(g_state.fd);
    g_state.fd = -1;
    return false;
  }
  g_state.bytes_since_sync.store(sizeof(header) - 1,
                                  std::memory_order_relaxed);
  g_state.last_sync = std::chrono::steady_clock::now();
  g_state.active.store(true);
  return true;
}

bool is_active() { return g_state.active.load(); }

void *new_file_ctx(const char *path) {
  if (!g_state.active.load()) return nullptr;
  auto *h = new file_ctx_holder_t();
  auto &alloc = h->doc.GetAllocator();
  rapidjson::Value key("path", alloc);
  rapidjson::Value val(path, alloc);
  h->doc.AddMember(key, val, alloc);
  return h;
}

void set_string(void *file_ctx, const char *key, const char *value) {
  if (file_ctx == nullptr) return;
  auto *h = as_holder(file_ctx);
  auto &alloc = h->doc.GetAllocator();
  rapidjson::Value k(key, alloc);
  rapidjson::Value v(value, alloc);
  h->doc.AddMember(k, v, alloc);
}

void set_uint64(void *file_ctx, const char *key, uint64_t value) {
  if (file_ctx == nullptr) return;
  auto *h = as_holder(file_ctx);
  auto &alloc = h->doc.GetAllocator();
  rapidjson::Value k(key, alloc);
  h->doc.AddMember(k, rapidjson::Value(value), alloc);
}

void set_uint32(void *file_ctx, const char *key, uint32_t value) {
  if (file_ctx == nullptr) return;
  auto *h = as_holder(file_ctx);
  auto &alloc = h->doc.GetAllocator();
  rapidjson::Value k(key, alloc);
  h->doc.AddMember(k, rapidjson::Value(value), alloc);
}

void record_sparse_chunks(void *file_ctx, uint64_t file_logical_offset,
                          size_t sparse_map_size,
                          const ds_sparse_chunk_t *sparse_map) {
  if (file_ctx == nullptr || sparse_map_size == 0 || sparse_map == nullptr) {
    return;
  }
  auto *h = as_holder(file_ctx);
  auto &alloc = h->doc.GetAllocator();

  /* Find-or-create the "sparse_map" array on the document. */
  rapidjson::Value *arr = nullptr;
  if (h->doc.HasMember("sparse_map")) {
    arr = &h->doc["sparse_map"];
  } else {
    rapidjson::Value key("sparse_map", alloc);
    rapidjson::Value v(rapidjson::kArrayType);
    h->doc.AddMember(key, v, alloc);
    arr = &h->doc["sparse_map"];
  }

  /* Walk the per-call sparse_map: each entry is {skip bytes of hole,
  len bytes of data}.  We record holes only (non-zero skip).  The
  caller-supplied file_logical_offset is where the buffer would start
  in the unpacked file; we add skip+len as we move along. */
  uint64_t off = file_logical_offset;
  for (size_t i = 0; i < sparse_map_size; ++i) {
    const size_t skip = sparse_map[i].skip;
    const size_t len = sparse_map[i].len;
    if (skip > 0) {
      rapidjson::Value entry(rapidjson::kObjectType);
      entry.AddMember("offset", rapidjson::Value(off), alloc);
      entry.AddMember("length", rapidjson::Value(uint64_t{skip}), alloc);
      arr->PushBack(entry, alloc);
    }
    off += skip + len;
  }
}

void *open_section(void *file_ctx, const char *name) {
  if (file_ctx == nullptr) return nullptr;
  auto *h = as_holder(file_ctx);
  auto &alloc = h->doc.GetAllocator();
  rapidjson::Value key(name, alloc);
  rapidjson::Value section(rapidjson::kObjectType);
  h->doc.AddMember(key, section, alloc);
  /* AddMember moves; fetch the inserted value back so the caller
  can mutate it via the returned pointer. */
  return &h->doc[name];
}

void append_and_release(void *file_ctx) {
  if (file_ctx == nullptr) return;
  auto *h = as_holder(file_ctx);

  if (!g_state.active.load()) {
    delete h;
    return;
  }

  /* Serialise to a string buffer + trailing newline. */
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
  h->doc.Accept(writer);
  const size_t json_len = buf.GetSize();

  /* Common case: short line, atomic O_APPEND write. */
  if (json_len + 1 <= kPipeBuf) {
    char stack_buf[kPipeBuf];
    memcpy(stack_buf, buf.GetString(), json_len);
    stack_buf[json_len] = '\n';
    const ssize_t n = ::write(g_state.fd, stack_buf, json_len + 1);
    if (n != (ssize_t)(json_len + 1)) {
      msg("xb_files_jsonl::append_and_release: short write\n");
    }
    delete h;
    maybe_sync(json_len + 1);
    return;
  }

  /* Rare path: line longer than PIPE_BUF.  Hold a mutex so the
  multi-call write stays atomic with respect to other appenders. */
  std::lock_guard<std::mutex> lk(g_state.big_line_mutex);
  const char *src = buf.GetString();
  size_t remaining = json_len;
  while (remaining > 0) {
    const ssize_t n = ::write(g_state.fd, src, remaining);
    if (n <= 0) {
      msg("xb_files_jsonl::append_and_release: big-line write failed\n");
      delete h;
      return;
    }
    src += n;
    remaining -= n;
  }
  const char nl = '\n';
  ::write(g_state.fd, &nl, 1);
  delete h;
  maybe_sync(json_len + 1);
}

void finalize() {
  if (!g_state.active.load()) return;
  if (g_state.fd >= 0) {
    fdatasync(g_state.fd);
    ::close(g_state.fd);
    g_state.fd = -1;
  }
  g_state.active.store(false);
}

bool publish(ds_ctxt_t *ds_root) {
  if (g_state.staging_path.empty()) {
    msg("xb_files_jsonl::publish: nothing to publish (no staging path)\n");
    return false;
  }

  ds_file_t *out =
      ds_open_single_object(ds_root, XB_BACKUP_FILES_JSONL, nullptr);
  if (out == nullptr) {
    msg("xb_files_jsonl::publish: ds_open_single_object failed for %s\n",
        XB_BACKUP_FILES_JSONL);
    return false;
  }

  std::ifstream in(g_state.staging_path, std::ios::binary);
  if (!in) {
    msg("xb_files_jsonl::publish: failed to open staging file %s for read\n",
        g_state.staging_path.c_str());
    ds_close(out);
    return false;
  }

  char buf[64 * 1024];
  while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
    if (ds_write(out, buf, (size_t)in.gcount()) != 0) {
      msg("xb_files_jsonl::publish: ds_write failed\n");
      ds_close(out);
      return false;
    }
  }
  if (ds_close(out) != 0) {
    msg("xb_files_jsonl::publish: ds_close failed\n");
    return false;
  }
  return true;
}

void cleanup_staging() {
  if (g_state.staging_path.empty()) return;
  ::unlink(g_state.staging_path.c_str());
  g_state.staging_path.clear();
}

bool write_to_dir(const char *dir) {
  if (g_state.staging_path.empty()) return false;
  std::ifstream in(g_state.staging_path, std::ios::binary);
  if (!in) {
    msg("xb_files_jsonl::write_to_dir: cannot open staging %s\n",
        g_state.staging_path.c_str());
    return false;
  }
  std::string out_path = dir;
  out_path += "/";
  out_path += XB_BACKUP_FILES_JSONL;
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    msg("xb_files_jsonl::write_to_dir: cannot open %s for write\n",
        out_path.c_str());
    return false;
  }
  out << in.rdbuf();
  return out.good();
}

}  // namespace xb_files_jsonl
