/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

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

/* Storage probe for --page-tracking-combine-distance=auto (PXB-3862).

Measures the two device characteristics the read request cost
needs (see read_request_cost_bytes() below): the per-request round trip, from
scattered single-page-sized reads, and the sequential read bandwidth, from a few
large contiguous reads. ~28 reads / ~16MB total, well under a second on
any storage.

Plain POSIX on purpose: no server I/O layer, no xtrabackup globals, so
the probe is unit-testable standalone and can be pointed at any file -
including a live mysqld datadir - by the env-gated gunit case in
unittest/gunit/innodb/xb_page_group-t.cc. */

#ifndef XB_IO_PROBE_H
#define XB_IO_PROBE_H

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace pagetracking {

/** Files smaller than this cannot be measured meaningfully (the
sequential samples would not reach steady state); the probe rejects
them and the caller keeps the fallback read request cost. */
constexpr uint64_t PROBE_MIN_FILE_BYTES = 64 * 1024 * 1024;

/** Result of probe_storage(); valid is false when the file could not
be opened, is smaller than PROBE_MIN_FILE_BYTES, or a read failed. */
struct Probe_result {
  uint64_t rtt_us{0};           /*!< per-request round trip, median */
  uint64_t bw_bytes_per_sec{0}; /*!< sequential read bandwidth */
  bool valid{false};
};

/** Measure the storage behind a file.
@param[in] path          file to probe (a large data file of the backup
                         source, so the numbers describe the same device
                         the copy will read)
@param[in] use_o_direct  open with O_DIRECT, matching how the backup
                         will read the data files; without it, page
                         cache hits can understate the round trip (the
                         resulting cost is then smaller - the safe
                         direction)
@return measured characteristics; see Probe_result */
inline Probe_result probe_storage(const char *path, bool use_o_direct) {
  using clock = std::chrono::steady_clock;
  using std::chrono::duration_cast;
  using std::chrono::microseconds;

  constexpr int RTT_SAMPLES = 12;
  constexpr uint64_t RTT_READ_BYTES = 16 * 1024;
  constexpr uint64_t SEQ_CHUNK_BYTES = 4 * 1024 * 1024;
  constexpr int SEQ_SAMPLES = 4;
  constexpr size_t BUF_ALIGN = 4096; /* covers any O_DIRECT block size */

  Probe_result result;

  int flags = O_RDONLY;
#ifdef O_DIRECT
  if (use_o_direct) {
    flags |= O_DIRECT;
  }
#endif

  const int fd = ::open(path, flags);
  if (fd < 0) {
    return (result);
  }

  struct stat file_stat;
  if (::fstat(fd, &file_stat) != 0 ||
      static_cast<uint64_t>(file_stat.st_size) < PROBE_MIN_FILE_BYTES) {
    ::close(fd);
    return (result);
  }
  const uint64_t file_size = file_stat.st_size;

  void *buf = nullptr;
  if (::posix_memalign(&buf, BUF_ALIGN, SEQ_CHUNK_BYTES) != 0) {
    ::close(fd);
    return (result);
  }

  /* scattered page-sized reads across the file -> round trip (median,
  so a stray stall or cache hit cannot skew the estimate) */
  std::vector<uint64_t> rtt_samples;
  for (int i = 1; i <= RTT_SAMPLES; i++) {
    uint64_t offset = (file_size / (RTT_SAMPLES + 1)) * i;
    offset -= offset % RTT_READ_BYTES;
    const auto start = clock::now();
    if (::pread(fd, buf, RTT_READ_BYTES, offset) !=
        static_cast<ssize_t>(RTT_READ_BYTES)) {
      ::free(buf);
      ::close(fd);
      return (result);
    }
    rtt_samples.push_back(
        duration_cast<microseconds>(clock::now() - start).count());
  }
  std::nth_element(rtt_samples.begin(),
                   rtt_samples.begin() + rtt_samples.size() / 2,
                   rtt_samples.end());
  result.rtt_us = std::max<uint64_t>(rtt_samples[rtt_samples.size() / 2], 1);

  /* contiguous chunks from the middle of the file -> bandwidth.
  PROBE_MIN_FILE_BYTES guarantees the chunks fit after file_size / 2. */
  uint64_t offset = file_size / 2;
  offset -= offset % RTT_READ_BYTES;
  uint64_t seq_bytes = 0;
  const auto start = clock::now();
  for (int i = 0; i < SEQ_SAMPLES; i++) {
    const ssize_t n_read = ::pread(fd, buf, SEQ_CHUNK_BYTES, offset);
    if (n_read <= 0) {
      break;
    }
    seq_bytes += n_read;
    offset += n_read;
  }
  const uint64_t seq_us = std::max<uint64_t>(
      duration_cast<microseconds>(clock::now() - start).count(), 1);

  ::free(buf);
  ::close(fd);

  if (seq_bytes == 0) {
    return (result);
  }

  result.bw_bytes_per_sec = seq_bytes * 1000000 / seq_us;
  result.valid = true;
  return (result);
}

/** clamp bounds for the measured read request cost */
constexpr uint64_t READ_REQUEST_COST_MIN_BYTES = 64 * 1024;
constexpr uint64_t READ_REQUEST_COST_MAX_BYTES = 1024 * 1024;

/** Fallback read request cost when the storage could not be probed: a
conservative cut across common storage classes. */
constexpr uint64_t READ_REQUEST_COST_FALLBACK_BYTES = 512 * 1024;

/** Compute the read request cost: what one read request costs the
backup, expressed in bytes of sequential transfer. It bounds gap
combining - the most bytes worth reading across one gap of unchanged
pages to save one read request - and is derived from measured storage
characteristics.

Combining across a gap saves one request round trip (rtt) and costs
gap_bytes / bandwidth of transfer, so the raw break-even is
rtt * bandwidth. Two thirds of that is used: the probe measures raw
device bandwidth, but filler bytes also flow through the copy
pipeline (checksum, buffer management) whose effective bandwidth is
lower, and the risk is asymmetric - too large a cost makes backups
slower than strict grouping (a regression), too small only misses
part of the win. The margin was calibrated on two instrumented
machines: a shared-disk NVMe (raw 171KB, true break-even ~131KB:
measured, combining across 144KB gaps loses) needs a divisor >= ~1.3;
a split-data-and-backup-disk server (raw ~245KB, effective ~= raw:
combining across 144KB gaps wins) needs the cost above 144KB. Dividing by
1.5 satisfies both with margin; dividing by 2 was over-conservative
and measurably refused profitable merges on the split-disk machine.

Reference points at /1.5: local NVMe 0.15ms x 1.2GB/s -> ~115KB
(still refuses the merges that regress there); split-disk server
0.165ms x 1.5GB/s -> ~165KB (combines the 144KB gaps that win there);
~1ms cloud volume x 400MB/s -> ~250KB; HDD 8ms x 150MB/s -> ~800KB.

@param[in] rtt_us            measured per-request round trip, microseconds
@param[in] bw_bytes_per_sec  measured sequential read bandwidth
@return read request cost in bytes, clamped to
        [READ_REQUEST_COST_MIN_BYTES, READ_REQUEST_COST_MAX_BYTES] */
inline uint64_t read_request_cost_bytes(uint64_t rtt_us,
                                        uint64_t bw_bytes_per_sec) {
  const uint64_t raw = rtt_us * bw_bytes_per_sec / 1000000 * 2 / 3;
  if (raw < READ_REQUEST_COST_MIN_BYTES) {
    return (READ_REQUEST_COST_MIN_BYTES);
  }
  if (raw > READ_REQUEST_COST_MAX_BYTES) {
    return (READ_REQUEST_COST_MAX_BYTES);
  }
  return (raw);
}

}  // namespace pagetracking

#endif /* XB_IO_PROBE_H */
