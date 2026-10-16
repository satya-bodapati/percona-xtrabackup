/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Table selection for xtrabackup --restore and xbcloud get.

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

#ifndef XTRABACKUP_RESTORE_FILTER_H
#define XTRABACKUP_RESTORE_FILTER_H

#include <string>
#include <vector>

namespace xb {
namespace restore {

/** Match a glob pattern against a string. '*' matches any sequence, '?'
matches one character, and a backslash makes the next character literal.
Characters are compared byte by byte, so a multi-byte character in the text
needs a '*' or as many '?' as it has bytes.
@param[in]	pattern			glob pattern
@param[in]	text			string to match
@param[in]	case_insensitive	compare ASCII letters ignoring case
@return true if the whole text matches the whole pattern */
bool glob_match(const std::string &pattern, const std::string &text,
                bool case_insensitive);

/** Decode a name as the server stores it on disk (the "filename" character
set, where for example '-' is written "@002d") into the name SQL uses, in
utf8mb4.
@param[in]	fs_name		encoded name
@param[out]	name		decoded name
@return true on success, false if fs_name is not valid in that encoding */
bool decode_fs_name(const std::string &fs_name, std::string *name);

/** Find out which table a file inside a backup belongs to.

Handles the data file of a file-per-table tablespace and the files a backup
keeps next to it: "db/t1.ibd", a partition "db/t1#p#p0.ibd" or
"db/t1#p#p0#sp#s0.ibd", the incremental "db/t1.ibd.delta" and "db/t1.ibd.meta",
the reduced-lock "db/t1.ibd.new" (with .delta and .meta) and "db/t1.ibd.crpt",
each optionally followed by the compression and encryption suffixes ".zst",
".lz4", ".qp" and ".xbcrypt". FULLTEXT auxiliary tablespaces and intermediate
"#sql" tables belong to no table the user can select, so they are not
recognised.
@param[in]	path		path relative to the backup root, '/' separated
@param[out]	fs_db		database directory name, still encoded
@param[out]	fs_table	table name without partition suffix, still
encoded
@return true if the path is a file of a user table */
bool parse_table_file(const std::string &path, std::string *fs_db,
                      std::string *fs_table);

/** Whether a file inside a backup is needed to prepare and restore any table
from it: everything at the backup root except general tablespaces, that is
the system tablespace, the undo tablespaces, mysql.ibd, the redo copy, the
metadata and keyring files, plus the reduced-lock markers (".ren", ".del").
@param[in]	path	path relative to the backup root, '/' separated
@return true if the file belongs to the set every restore needs */
bool is_core_file(const std::string &path);

/** The tables a restore selects, from --restore-include and --restore-exclude.

Patterns name tables as db.table on real names, the way SQL shows them. Both
parts are globs (see glob_match()). A pattern without a dot names a whole
database. A table is selected when it matches at least one include pattern and
no exclude pattern. Only tables are selected: the partitions of a table always
follow it. */
class Table_filter {
 public:
  /** Add patterns.
  @param[in]	list		comma separated patterns
  @param[in]	include		true for --restore-include, false for
                                --restore-exclude
  @param[out]	error		why the list was refused
  @return true on success */
  bool add_list(const std::string &list, bool include, std::string *error);

  /** Add patterns from a file, one per line. Empty lines and lines starting
  with '#' are skipped.
  @param[in]	path		file to read
  @param[in]	include		true for include patterns, false for exclude
  @param[out]	error		why the file was refused
  @return true on success */
  bool add_file(const std::string &path, bool include, std::string *error);

  /** Compare ASCII letters in names ignoring case, as lower_case_table_names
  1 and 2 do.
  @param[in]	value	true to ignore case */
  void set_case_insensitive(bool value) { m_case_insensitive = value; }

  /** @return true if at least one include pattern was given */
  bool active() const { return !m_include.empty(); }

  /** Whether a table is selected. Remembers which include patterns selected
  something, for unused_includes().
  @param[in]	db	database name, decoded
  @param[in]	table	table name, decoded
  @return true if selected */
  bool select(const std::string &db, const std::string &table);

  /** Same as select() for an encoded database directory and table name.
  Names that cannot be decoded are not selected.
  @param[in]	fs_db		database directory name, encoded
  @param[in]	fs_table	table name, encoded
  @return true if selected */
  bool select_fs(const std::string &fs_db, const std::string &fs_table);

  /** @return the include patterns that selected no table so far */
  std::vector<std::string> unused_includes() const;

 private:
  /** One pattern, split into its database and table parts. */
  struct Pattern {
    /** pattern as the user wrote it */
    std::string text;
    /** glob for the database name */
    std::string db;
    /** glob for the table name */
    std::string table;
    /** whether it selected at least one table */
    bool used{false};
  };

  /** Parse one pattern and add it.
  @param[in]	text		pattern
  @param[in]	include		true for an include pattern
  @param[out]	error		why it was refused
  @return true on success */
  bool add_one(const std::string &text, bool include, std::string *error);

  /** Whether a pattern matches a table.
  @param[in]	p	pattern
  @param[in]	db	database name
  @param[in]	table	table name
  @return true on match */
  bool matches(const Pattern &p, const std::string &db,
               const std::string &table) const;

  /** include patterns */
  std::vector<Pattern> m_include;
  /** exclude patterns */
  std::vector<Pattern> m_exclude;
  /** compare names ignoring ASCII case */
  bool m_case_insensitive{false};
};

}  // namespace restore
}  // namespace xb

#endif /* XTRABACKUP_RESTORE_FILTER_H */
