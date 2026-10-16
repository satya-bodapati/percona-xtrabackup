/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

xtrabackup --restore: restore single tables or databases from a prepared
backup into a running Percona Server on the same host.

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

#ifndef XTRABACKUP_RESTORE_H
#define XTRABACKUP_RESTORE_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/*
How --restore works

1. Before InnoDB starts: parse the selection, connect to the server and check
   that it can take the restore (init()).
2. With InnoDB up on the backup, the way --prepare runs it: read the definition
   of every selected table from the SDI of its .ibd, its triggers from
   mysql.triggers, its database from mysql.schemata, its AUTO_INCREMENT
   counter, and write a .cfp for each encrypted tablespace (collect()).
3. With InnoDB shut down again: for each table, copy the files into the
   server's datadir and run IMPORT TABLE FROM, then put back what the .ibd does
   not carry (run()).

Steps 1 and 3 talk to the server; step 2 only reads the backup.
*/

/** Exit codes of --restore. */
enum restore_exit_t {
  /** every selected table was restored without warnings */
  RESTORE_EXIT_OK = 0,
  /** some tables failed, or some were restored with warnings */
  RESTORE_EXIT_PARTIAL = 1,
  /** nothing was restored: the run stopped before the first table */
  RESTORE_EXIT_FATAL = 2
};

/** --restore */
extern bool xtrabackup_restore;
/** --restore-include */
extern char *opt_restore_include;
/** --restore-exclude */
extern char *opt_restore_exclude;
/** --restore-include-file */
extern char *opt_restore_include_file;
/** --restore-exclude-file */
extern char *opt_restore_exclude_file;
/** --restore-report */
extern char *opt_restore_report;
/** --dry-run */
extern bool opt_restore_dry_run;

struct fil_space_t;

/** Write the encryption data of a tablespace to a .cfp file, in the format
FLUSH TABLES ... FOR EXPORT and --export use. Defined in xtrabackup.cc.
@param[in]	space	encrypted tablespace
@param[in]	path	file to write
@return true on success */
bool xb_write_cfp_file(const fil_space_t *space, const char *path);

namespace xb {
namespace restore {

/** Step 1: read the selection options, connect to the server and check it.
@return true if the restore can go on */
bool init();

/** Step 2, called by --prepare once InnoDB is up on the backup.
@return true unless the backup could not be read at all */
bool collect();

/** Step 3: restore the collected tables (or, with --dry-run, list them).
@return exit code, one of restore_exit_t */
int run();

}  // namespace restore
}  // namespace xb

#endif /* XTRABACKUP_RESTORE_H */
