/******************************************************
Copyright (c) 2021 Percona LLC and/or its affiliates.

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
#include <my_alloc.h>
#include <my_default.h>
#include <mysqld.h>

#ifdef __APPLE__
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#else
/* Pick the procps headers based on what cmake/procps.cmake detected.
 * Be explicit about each supported version: an unrecognised / missing
 * macro should be a loud compile error rather than silently falling
 * into the V4 branch. To add support for a future procps version,
 * extend cmake/procps.cmake to define HAVE_PROCPS_V<N> and add a
 * matching #elif branch both here and in meminfo_kb() below. */
#if defined(HAVE_PROCPS_V3)
#include <proc/sysinfo.h>
#elif defined(HAVE_PROCPS_V4)
#include <libproc2/meminfo.h>
#else
#error \
    "xtrabackup utils.cc: no supported procps version detected." \
    " Define HAVE_PROCPS_V3 (libprocps) or HAVE_PROCPS_V4 (libproc2) ; see cmake/procps.cmake."
#endif
#endif                                     // __APPLE__
#include <boost/uuid/uuid.hpp>             // uuid class
#include <boost/uuid/uuid_generators.hpp>  // generators
#include <boost/uuid/uuid_io.hpp>          // streaming operators etc.
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include "common.h"
#include "msg.h"
#include "xtrabackup.h"

static boost::uuids::random_generator gen = boost::uuids::random_generator();

namespace xtrabackup {
namespace utils {

bool load_backup_my_cnf(my_option *options, char *path) {
  static MEM_ROOT argv_alloc{PSI_NOT_INSTRUMENTED, 512};
  const char *groups[] = {"mysqld", NULL};

  char *exename = (char *)"xtrabackup";
  char **backup_my_argv = &exename;
  int backup_my_argc = 1;
  char config_file[FN_REFLEN];

  /* we need full name so that only backup-my.cnf will be read */
  if (fn_format(config_file, "backup-my.cnf", path, "",
                MY_UNPACK_FILENAME | MY_SAFE_PATH) == NULL) {
    return (false);
  }

  if (my_load_defaults(config_file, groups, &backup_my_argc, &backup_my_argv,
                       &argv_alloc, NULL)) {
    return (false);
  }

  if (handle_options(&backup_my_argc, &backup_my_argv, options, NULL)) {
    return (false);
  }

  return (true);
}

bool read_server_uuid() {
  char *uuid = NULL;
  bool ret;
  my_option config_options[] = {
      {"server-uuid", 0, "", &uuid, &uuid, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0,
       0, 0},
      {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};
  if (xtrabackup_incremental_dir != nullptr) {
    ret = xtrabackup::utils::load_backup_my_cnf(config_options,
                                                xtrabackup_incremental_dir);
  } else {
    ret = xtrabackup::utils::load_backup_my_cnf(config_options,
                                                xtrabackup_real_target_dir);
  }
  if (!ret) {
    msg("xtrabackup: Error: failed to load backup-my.cnf\n");
    return (false);
  }
  memset(server_uuid, 0, Encryption::SERVER_UUID_LEN + 1);
  if (uuid != NULL) {
    strncpy(server_uuid, uuid, Encryption::SERVER_UUID_LEN);
  }
  return (true);
}

/* find the pxb base version */
unsigned long get_version_number(std::string version_str) {
  unsigned long major = 0, minor = 0, version = 0;
  std::size_t major_p = version_str.find(".");
  if (major_p != std::string::npos)
    major = stoi(version_str.substr(0, major_p));

  std::size_t minor_p = version_str.find(".", major_p + 1);
  if (minor_p != std::string::npos)
    minor = stoi(version_str.substr(major_p + 1, minor_p - major_p));

  std::size_t version_p = version_str.find(".", minor_p + 1);
  if (version_p != std::string::npos)
    version = stoi(version_str.substr(minor_p + 1, version_p - minor_p));
  else
    version = stoi(version_str.substr(minor_p + 1));
  return major * 10000 + minor * 100 + version;
}

bool get_major_minor_version(const std::string &version_str,
                             std::string &version) {
  unsigned long major = 0, minor = 0;

  std::size_t major_p = version_str.find(".");
  if (major_p == std::string::npos) return false;

  major = stoi(version_str.substr(0, major_p));

  std::size_t minor_p = version_str.find(".", major_p + 1);
  if (minor_p == std::string::npos) return false;

  minor = stoi(version_str.substr(major_p + 1, minor_p - major_p));

  version = std::to_string(major) + "." + std::to_string(minor);
  return true;
}

#ifdef __APPLE__
unsigned long host_total_memory() {
  unsigned long total_mem = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
  return total_mem;
}

unsigned long host_free_memory() {
  unsigned long total_mem = host_total_memory();
  int64_t used_mem;
  vm_size_t page_size;
  mach_msg_type_number_t count;
  vm_statistics_data_t vm_stats;

  // Get used memory
  mach_port_t host = mach_host_self();
  count = sizeof(vm_stats) / sizeof(natural_t);
  if (KERN_SUCCESS == host_page_size(host, &page_size) &&
      KERN_SUCCESS ==
          host_statistics(host, HOST_VM_INFO, (host_info_t)&vm_stats, &count)) {
    used_mem = ((int64_t)vm_stats.active_count + (int64_t)vm_stats.wire_count) *
               (int64_t)page_size;

    ut_a(total_mem >= (unsigned long)used_mem);
    return total_mem - (unsigned long)used_mem;
  }
  return 0;
}
#else /* !__APPLE__ - Linux / procps */

/* meminfo_kind selects which /proc/meminfo field to retrieve. The
 * version-specific implementation of meminfo_kb() below maps it to
 * the appropriate procps API call.
 *
 * Per-version code is intentionally confined to meminfo_kb(); the
 * public host_total_memory() / host_free_memory() entry points stay
 * version-agnostic. Adding support for a new procps version (e.g. a
 * future V5) means adding one #elif branch to meminfo_kb() and
 * nothing else. */
enum class meminfo_kind { total, available };

#if defined(HAVE_PROCPS_V3)
/* libprocps (procps-ng 3.x). meminfo() seeds the kb_main_* globals
 * from /proc/meminfo; we then read whichever one the caller asked
 * for. */
static unsigned long meminfo_kb(meminfo_kind kind) {
  meminfo();
  return (kind == meminfo_kind::total) ? kb_main_total : kb_main_available;
}
#elif defined(HAVE_PROCPS_V4)
/* libproc2 (procps-ng 4.x).
 *
 * MEMINFO_GET() / procps_meminfo_get() do NOT seed the cache by
 * themselves; they return whatever is currently stored in the
 * meminfo_info struct, and a freshly allocated struct holds zeroes.
 * Calling MEMINFO_GET() right after procps_meminfo_new() therefore
 * silently returns 0 - this is the trigger half of PXB-3770
 * (--use-free-memory-pct=N then computes 0% available, feeds 0 into
 * buf_pool_size_align_down(), unsigned-underflows the buffer pool
 * size, and SIGSEGVs InnoDB).
 *
 * procps_meminfo_select() is the documented "fresh-read, batch-fetch"
 * primitive of libproc2; it performs an internal /proc/meminfo read
 * and returns a stack with results in the order requested. It is the
 * V4 analogue of what the V3 path does via meminfo() before reading
 * the kb_main_* globals.
 *
 * Returns the requested item in kB, or 0 on failure. */
static unsigned long meminfo_kb(meminfo_kind kind) {
  struct meminfo_info *mem_info = nullptr;
  if (procps_meminfo_new(&mem_info) < 0) {
    return 0;
  }
  enum meminfo_item items[] = {(kind == meminfo_kind::total)
                                   ? MEMINFO_MEM_TOTAL
                                   : MEMINFO_MEM_AVAILABLE};
  struct meminfo_stack *stack = procps_meminfo_select(mem_info, items, 1);
  unsigned long kb = (stack != nullptr) ? stack->head[0].result.ul_int : 0;
  procps_meminfo_unref(&mem_info);
  return kb;
}
#else
/* Belt-and-braces: the include block above already errors out for
 * unknown procps versions, but make the failure trigger at this
 * point too - if someone copies this file or splits the includes
 * apart, the helper still won't silently disappear. */
#error \
    "xtrabackup utils.cc: no supported procps version detected." \
    " Define HAVE_PROCPS_V3 (libprocps) or HAVE_PROCPS_V4 (libproc2) ; see cmake/procps.cmake."
#endif

unsigned long host_total_memory() {
  return meminfo_kb(meminfo_kind::total) * 1024;
}

unsigned long host_free_memory() {
  return meminfo_kb(meminfo_kind::available) * 1024;
}
#endif /* __APPLE__ */

std::string generate_uuid() {
  boost::uuids::uuid uuid = gen();
  std::ostringstream uuid_ss;
  uuid_ss << uuid;
  return uuid_ss.str();
}

std::string formatElapsedTime(std::chrono::nanoseconds elapsed) {
  using namespace std::chrono;

  auto days = duration_cast<duration<int, std::ratio<86400>>>(elapsed);
  elapsed -= days;
  auto hours = duration_cast<std::chrono::hours>(elapsed);
  elapsed -= hours;
  auto minutes = duration_cast<std::chrono::minutes>(elapsed);
  elapsed -= minutes;
  auto seconds = duration_cast<std::chrono::seconds>(elapsed);
  elapsed -= seconds;
  auto milliseconds = duration_cast<std::chrono::milliseconds>(elapsed);
  elapsed -= milliseconds;
  auto microseconds = duration_cast<std::chrono::microseconds>(elapsed);
  elapsed -= microseconds;
  auto nanoseconds = elapsed;

  std::ostringstream result;
  if (days.count() > 0) {
    result << days.count() << " days ";
    if (hours.count() > 0) result << hours.count() << " hr ";
    if (minutes.count() > 0) result << minutes.count() << " mins";
  } else if (hours.count() > 0) {
    double fractionalMinutes = minutes.count() + (seconds.count() / 60.0);
    result << std::fixed << std::setprecision(2)
           << hours.count() + (fractionalMinutes / 60.0) << " hours";
  } else if (minutes.count() > 0) {
    double fractionalSeconds =
        seconds.count() + (milliseconds.count() / 1000.0);
    result << std::fixed << std::setprecision(2)
           << minutes.count() + (fractionalSeconds / 60.0) << " minutes";
  } else if (seconds.count() > 0) {
    double fractionalMilliseconds =
        milliseconds.count() / 1000.0 + (microseconds.count() / 1000000.0);
    result << std::fixed << std::setprecision(3)
           << seconds.count() + fractionalMilliseconds << " seconds";
  } else if (milliseconds.count() > 0) {
    double fractionalMilliseconds = milliseconds.count() +
                                    (microseconds.count() / 1000.0) +
                                    (nanoseconds.count() / 1000000.0);
    result << std::fixed << std::setprecision(3) << fractionalMilliseconds
           << " milliseconds";
  } else if (microseconds.count() > 0) {
    double fractionalMicroseconds =
        microseconds.count() + (nanoseconds.count() / 1000.0);
    result << std::fixed << std::setprecision(3) << fractionalMicroseconds
           << " microseconds";
  } else if (nanoseconds.count() > 0) {
    // Display nanoseconds as is, since this is the smallest unit
    result << nanoseconds.count() << " nanoseconds";
  } else {
    // If everything is zero, return 0 nanoseconds (edge case handling)
    result << "0 nanoseconds";
  }

  return result.str();
}

std::string human_readable(unsigned long long bytes) {
  char buf[64];
  if (bytes >= 1ULL << 30)
    snprintf(buf, sizeof(buf), "%.2f GiB", (double)bytes / (1ULL << 30));
  else if (bytes >= 1ULL << 20)
    snprintf(buf, sizeof(buf), "%.2f MiB", (double)bytes / (1ULL << 20));
  else if (bytes >= 1ULL << 10)
    snprintf(buf, sizeof(buf), "%.2f KiB", (double)bytes / (1ULL << 10));
  else
    snprintf(buf, sizeof(buf), "%llu bytes", bytes);
  return buf;
}

}  // namespace utils
}  // namespace xtrabackup
