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

#include "restore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

#include <my_rapidjson_size_t.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <errmsg.h>
#include <my_sys.h>
#include "mysql/strings/m_ctype.h"
#include "sql/current_thd.h"
#include "sql/dd/properties.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/index_element.h"
#include "sql/dd/types/partition.h"
#include "sql/dd/types/table.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/fil0fil.h"
#include "storage/innobase/include/fsp0fsp.h"

#include "backup_conn.h"
#include "restore_filter.h"
#include "space_map.h"
#include "xb_dict.h"
#include "xtrabackup.h"

bool xtrabackup_restore = false;
char *opt_restore_include = nullptr;
char *opt_restore_exclude = nullptr;
char *opt_restore_include_file = nullptr;
char *opt_restore_exclude_file = nullptr;
char *opt_restore_report = nullptr;
bool opt_restore_dry_run = false;

/* Defined in xtrabackup.cc. */
extern char *opt_mysql_tmpdir;
extern char mysql_server_version_str[30];
extern char mysql_server_flavor[256];

namespace xb {
namespace restore {

/** A data file of a table: one for a plain table, one per partition. */
struct Table_file {
  /** tablespace name, as fil_space_t has it */
  std::string space_name;
  /** path relative to the backup directory */
  std::string backup_path;
  /** where the file goes on the server host, absolute */
  std::string target_path;
  /** how IMPORT TABLE names it: relative to the datadir, or absolute for a
  table with DATA DIRECTORY */
  std::string import_path;
  /** .cfp written while InnoDB ran on the backup; empty if not encrypted */
  std::string cfp_tmp_path;
  /** the .cfp next to target_path, for an encrypted tablespace */
  std::string cfp_target_path;
};

/** A FULLTEXT index, to rebuild after the import. */
struct Fts_index {
  /** index name */
  std::string name;
  /** indexed columns, in order */
  std::vector<std::string> columns;
  /** WITH PARSER name, empty for the built-in parser */
  std::string parser;
};

/** A table to restore, and what happened to it. */
struct Table {
  /** database name, decoded */
  std::string db;
  /** table name, decoded */
  std::string name;
  /** database directory name, encoded */
  std::string fs_db;
  /** table file name without extension or partition suffix, encoded */
  std::string fs_name;
  /** data files; partitions in backup order */
  std::vector<Table_file> files;
  /** whether the table is partitioned */
  bool partitioned{false};
  /** the directory holding <fs_db>/ for a table created with DATA DIRECTORY,
  empty for a table in the datadir */
  std::string data_dir;
  /** whether the tablespace is encrypted */
  bool encrypted{false};
  /** whether the table has an AUTO_INCREMENT column */
  bool has_autoinc{false};
  /** next AUTO_INCREMENT value in the backup, 0 if unknown */
  uint64_t autoinc{0};
  /** FULLTEXT indexes */
  std::vector<Fts_index> fts;
  /** triggers, in the order they must be created */
  std::vector<prepare::Trigger_row> triggers;
  /** id of the table in the backup's mysql.tables */
  uint64_t sdi_id{0};
  /** why the table can't be restored, found while reading the backup */
  std::string collect_error;

  /** the table is in the server now */
  bool restored{false};
  /** why it was not restored */
  std::string reason;
  /** what went wrong after the import; the table exists but is not complete */
  std::vector<std::string> warnings;
};

/** A database some selected table belongs to. */
struct Schema {
  /** database name, decoded */
  std::string name;
  /** default collation in the backup, empty if unknown */
  std::string collation;
  /** default character set in the backup, empty if unknown */
  std::string charset;
  /** DEFAULT ENCRYPTION in the backup */
  bool default_encryption{false};
  /** set if it could not be created: every table in it fails with this */
  std::string error;
};

/** What the server looks like, read once before anything else. */
struct Server_info {
  /** @@datadir, resolved, without trailing slash */
  std::string datadir;
  /** @@lower_case_table_names */
  unsigned long lower_case_table_names{0};
  /** directories InnoDB knows about, resolved: datadir, innodb_data_home_dir,
  innodb_undo_directory and innodb_directories */
  std::vector<std::string> known_dirs;
  /** innodb_undo_directory, resolved, if it is not inside the datadir */
  std::string unique_undo_dir;
};

/** Selection from --restore-include and --restore-exclude. */
static Table_filter g_filter;
/** The tables selected, filled by collect(). */
static std::vector<Table> g_tables;
/** Their databases, by name. */
static std::map<std::string, Schema> g_schemas;
/** The server. */
static Server_info g_server;
/** Directory for the .cfp files written while InnoDB runs on the backup. */
static std::string g_tmpdir;

/** Quote an identifier with backticks.
@param[in]	name	identifier
@return quoted identifier */
static std::string quote_ident(const std::string &name) {
  std::string out = "`";
  for (char c : name) {
    if (c == '`') out.push_back('`');
    out.push_back(c);
  }
  out.push_back('`');
  return out;
}

/** Quote a string literal for a statement on a connection.
@param[in]	conn	connection the statement goes to
@param[in]	s	string
@return quoted literal */
static std::string quote_string(MYSQL *conn, const std::string &s) {
  std::string buf(s.size() * 2 + 1, '\0');
  const unsigned long len =
      mysql_real_escape_string_quote(conn, &buf[0], s.data(), s.size(), '\'');
  buf.resize(len);
  return "'" + buf + "'";
}

/** Escape the characters IMPORT TABLE takes as wildcards in a file name.
@param[in]	path	file name
@return file name that matches only itself */
static std::string escape_glob(const std::string &path) {
  std::string out;
  for (char c : path) {
    if (c == '*' || c == '?' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

/** @return "`db`.`table`" for a table */
static std::string qualified(const Table &t) {
  return quote_ident(t.db) + "." + quote_ident(t.name);
}

/** Run a statement and drop its result.
@param[in]	conn	connection
@param[in]	query	statement
@param[out]	error	the server's error message on failure
@return true on success */
static bool exec(MYSQL *conn, const std::string &query, std::string *error) {
  if (mysql_real_query(conn, query.data(), query.size()) != 0) {
    *error = mysql_error(conn);
    return false;
  }
  MYSQL_RES *res = mysql_store_result(conn);
  if (res != nullptr) {
    mysql_free_result(res);
  }
  return true;
}

/** Run a query and return its rows as strings; NULL becomes "".
@param[in]	conn	connection
@param[in]	query	query
@param[out]	rows	rows
@param[out]	error	the server's error message on failure
@return true on success */
static bool select_rows(MYSQL *conn, const std::string &query,
                        std::vector<std::vector<std::string>> *rows,
                        std::string *error) {
  if (mysql_real_query(conn, query.data(), query.size()) != 0) {
    *error = mysql_error(conn);
    return false;
  }
  MYSQL_RES *res = mysql_store_result(conn);
  if (res == nullptr) {
    *error = mysql_error(conn);
    return false;
  }
  const unsigned int n = mysql_num_fields(res);
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res)) != nullptr) {
    std::vector<std::string> r;
    for (unsigned int i = 0; i < n; ++i) {
      r.emplace_back(row[i] != nullptr ? row[i] : "");
    }
    rows->push_back(std::move(r));
  }
  mysql_free_result(res);
  return true;
}

/** Resolve a path to an absolute one without symbolic links. A path that does
not exist yet is resolved through its closest existing ancestor.
@param[in]	path	path
@return resolved path without trailing slash, or path itself if nothing of
it exists */
static std::string resolve_path(const std::string &path) {
  char buf[PATH_MAX];
  if (realpath(path.c_str(), buf) != nullptr) {
    return buf;
  }
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return path;
  }
  return resolve_path(path.substr(0, slash)) + path.substr(slash);
}

/** Whether a directory is a path or lies below it.
@param[in]	dir	resolved directory
@param[in]	path	resolved path
@return true if path is dir or inside it */
static bool is_same_or_under(const std::string &dir, const std::string &path) {
  if (dir.empty()) return false;
  if (path == dir) return true;
  return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
         (dir.back() == '/' || path[dir.size()] == '/');
}

/** Decide whether a server is Percona Server 8.4. Official builds say so in
version_comment; source builds say "Source distribution", but keep the
Percona release number in the version, as in 8.4.11-11.
@param[in]  version  server version, e.g. 8.4.11-11 or 8.4.11-11-debug
@param[in]  comment  version_comment, or the backup's server flavor
@return true if it is Percona Server 8.4 */
static bool is_percona_server_84(const std::string &version,
                                 const std::string &comment) {
  if (version.compare(0, 4, "8.4.") != 0) {
    return false;
  }

  if (comment.find("Percona") != std::string::npos) {
    return true;
  }

  /* 8.4.<patch>-<release>[-suffix] */
  size_t pos = 4;
  const size_t patch_end = version.find_first_not_of("0123456789", pos);
  if (patch_end == pos || patch_end == std::string::npos ||
      version[patch_end] != '-') {
    return false;
  }
  pos = patch_end + 1;
  const size_t release_end = version.find_first_not_of("0123456789", pos);
  return release_end != pos && comment.find("MariaDB") == std::string::npos;
}

/** Open a connection for the restore, set up for it: utf8mb4, nothing
written to the binary log, foreign keys not checked.
@param[in,out]	conn	connection to open
@return true on success */
static bool open_restore_connection(Connection &conn) {
  if (!connect(Purpose::RESTORE, conn)) {
    return false;
  }
  std::string error;
  for (const char *q :
       {"SET NAMES utf8mb4", "SET SESSION sql_log_bin = 0",
        "SET SESSION foreign_key_checks = 0", "SET SESSION autocommit = 1"}) {
    if (!exec(conn, q, &error)) {
      xb::error() << "--restore: " << q << " failed: " << error;
      return false;
    }
  }
  return true;
}

bool init() {
  std::string error;

  if (opt_restore_include == nullptr && opt_restore_include_file == nullptr) {
    xb::error() << "--restore needs --restore-include or "
                   "--restore-include-file to name the tables to restore";
    return false;
  }
  if ((opt_restore_include != nullptr &&
       !g_filter.add_list(opt_restore_include, true, &error)) ||
      (opt_restore_include_file != nullptr &&
       !g_filter.add_file(opt_restore_include_file, true, &error)) ||
      (opt_restore_exclude != nullptr &&
       !g_filter.add_list(opt_restore_exclude, false, &error)) ||
      (opt_restore_exclude_file != nullptr &&
       !g_filter.add_file(opt_restore_exclude_file, false, &error))) {
    xb::error() << "--restore: " << error;
    return false;
  }
  if (!g_filter.active()) {
    xb::error() << "--restore: the include patterns are empty";
    return false;
  }

  /* The backup must come from the same release series as the server: the
  data dictionary format of the SDI is checked again by the server. */
  if (!is_percona_server_84(mysql_server_version_str, mysql_server_flavor)) {
    xb::error() << "--restore: the backup was taken from "
                << mysql_server_flavor << " " << mysql_server_version_str
                << "; only backups of Percona Server 8.4 can be restored";
    return false;
  }

  Connection conn;
  if (!open_restore_connection(conn)) {
    return false;
  }

  std::vector<std::vector<std::string>> rows;
  if (!select_rows(conn,
                   "SELECT @@version, @@version_comment, @@datadir, "
                   "@@lower_case_table_names, @@innodb_data_home_dir, "
                   "@@innodb_undo_directory, @@innodb_directories, "
                   "@@server_uuid",
                   &rows, &error) ||
      rows.size() != 1) {
    xb::error() << "--restore: cannot read server variables: " << error;
    return false;
  }
  const auto &v = rows[0];

  if (!is_percona_server_84(v[0], v[1])) {
    xb::error() << "--restore: the server is " << v[1] << " " << v[0]
                << "; --restore needs Percona Server 8.4";
    return false;
  }

  g_server.datadir = resolve_path(v[2]);
  g_server.lower_case_table_names = strtoul(v[3].c_str(), nullptr, 10);
  g_filter.set_case_insensitive(g_server.lower_case_table_names != 0);

  /* The server must run on this host: its datadir has to be here, with the
  same server_uuid in auto.cnf. */
  struct stat st;
  if (stat((g_server.datadir + "/mysql.ibd").c_str(), &st) != 0) {
    xb::error() << "--restore: the server's datadir " << g_server.datadir
                << " is not on this host; --restore must run on the host of "
                   "the server it restores into";
    return false;
  }
  std::ifstream auto_cnf(g_server.datadir + "/auto.cnf");
  if (auto_cnf) {
    std::string line;
    while (std::getline(auto_cnf, line)) {
      if (line.compare(0, 12, "server-uuid=") == 0 && line.substr(12) != v[7]) {
        xb::error() << "--restore: " << g_server.datadir
                    << " belongs to another server than the one connected "
                       "to (server_uuid "
                    << v[7] << ")";
        return false;
      }
    }
  }

  auto add_known = [&](const std::string &dir) {
    if (dir.empty()) return;
    const std::string d =
        resolve_path(dir[0] == '/' ? dir : g_server.datadir + "/" + dir);
    g_server.known_dirs.push_back(d);
  };
  add_known(g_server.datadir);
  add_known(v[4]);
  add_known(v[5]);
  std::stringstream dirs(v[6]);
  std::string dir;
  while (std::getline(dirs, dir, ';')) {
    add_known(dir);
  }
  if (!v[5].empty()) {
    const std::string undo =
        resolve_path(v[5][0] == '/' ? v[5] : g_server.datadir + "/" + v[5]);
    if (!is_same_or_under(g_server.datadir, undo)) {
      g_server.unique_undo_dir = undo;
    }
  }

  /* The .cfp files of encrypted tables are written here while InnoDB runs on
  the backup. */
  std::string tmpl =
      std::string(opt_mysql_tmpdir) + "/xtrabackup_restore_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (mkdtemp(buf.data()) == nullptr) {
    xb::error() << "--restore: cannot create a directory in "
                << opt_mysql_tmpdir << ": " << strerror(errno);
    return false;
  }
  g_tmpdir = buf.data();

  xb::info() << "--restore: restoring into Percona Server " << v[0]
             << ", datadir " << g_server.datadir;
  return true;
}

/** Compare two table or database names the way the server does: ignoring
ASCII case with lower_case_table_names 1 or 2.
@param[in]	a	name
@param[in]	b	name
@return true if they name the same object */
static bool same_name(const std::string &a, const std::string &b) {
  if (g_server.lower_case_table_names == 0) {
    return a == b;
  }
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

/** Name of a collation, and of its character set.
@param[in]	id		collation id
@param[out]	collation	collation name
@param[out]	charset		character set name
@return true if the id is known */
static bool collation_names(uint64_t id, std::string *collation,
                            std::string *charset) {
  const CHARSET_INFO *cs = get_charset(static_cast<uint>(id), MYF(0));
  if (cs == nullptr) {
    return false;
  }
  *collation = cs->m_coll_name;
  *charset = cs->csname;
  return true;
}

/** One data file of a selected table, while collecting. */
struct Space_ref {
  /** tablespace id */
  space_id_t id;
  /** tablespace name */
  std::string name;
  /** path in the backup, relative */
  std::string path;
};

/** Fill a table from its definition in the backup.
@param[in]	def	definition read from the SDI
@param[in,out]	t	table */
static void describe_table(const dd::Table &def, Table *t) {
  t->partitioned = def.partition_type() != dd::Table::PT_NONE;

  for (const dd::Column *col : def.columns()) {
    if (col->is_auto_increment()) {
      t->has_autoinc = true;
    }
    /* Percona Server's COLUMN_FORMAT COMPRESSED (3). */
    uint32_t format = 0;
    if (col->options().exists("column_format") &&
        !col->options().get("column_format", &format) && format == 3) {
      t->collect_error =
          "tables with compressed columns are not supported until v2";
    }
  }

  for (const dd::Index *idx : def.indexes()) {
    if (idx->type() != dd::Index::IT_FULLTEXT) {
      continue;
    }
    Fts_index fts;
    fts.name.assign(idx->name().c_str(), idx->name().length());
    for (const dd::Index_element *e : idx->elements()) {
      if (e->is_hidden()) continue;
      fts.columns.emplace_back(e->column().name().c_str(),
                               e->column().name().length());
    }
    if (idx->options().exists("parser_name")) {
      dd::String_type parser;
      idx->options().get("parser_name", &parser);
      fts.parser.assign(parser.c_str(), parser.length());
    }
    t->fts.push_back(fts);
  }
}

bool collect() {
  THD *thd = current_thd;
  ut_a(thd != nullptr);

  /* Group the file-per-table tablespaces of the backup by table, keeping the
  selected ones. */
  std::map<std::pair<std::string, std::string>, std::vector<Space_ref>> groups;

  datafiles_iter_t *it = datafiles_iter_new(nullptr);
  if (it == nullptr) {
    xb::error() << "--restore: cannot list the tablespaces of the backup";
    return false;
  }
  fil_node_t *node;
  while ((node = datafiles_iter_next(it)) != nullptr) {
    fil_space_t *space = node->space;
    if (!fsp_is_file_per_table(space->id, space->flags)) {
      continue;
    }
    std::string path = node->name;
    if (path.compare(0, 2, "./") == 0) {
      path = path.substr(2);
    }
    std::string fs_db, fs_table;
    if (!parse_table_file(path, &fs_db, &fs_table) ||
        !g_filter.select_fs(fs_db, fs_table)) {
      continue;
    }
    groups[{fs_db, fs_table}].push_back({space->id, space->name, path});
  }
  datafiles_iter_free(it);

  const std::vector<std::string> unused = g_filter.unused_includes();
  if (!unused.empty()) {
    for (const auto &p : unused) {
      xb::error() << "--restore: pattern '" << p
                  << "' matches no table in the backup";
    }
    rmdir(g_tmpdir.c_str());
    return false;
  }

  std::vector<prepare::Schema_row> schema_rows;
  std::vector<prepare::Trigger_row> trigger_rows;
  if (prepare::scan_schemata(&schema_rows) != DB_SUCCESS ||
      prepare::scan_triggers(&trigger_rows) != DB_SUCCESS) {
    xb::error() << "--restore: cannot read the data dictionary of the backup";
    rmdir(g_tmpdir.c_str());
    return false;
  }

  unsigned cfp_count = 0;

  for (auto &g : groups) {
    Table t;
    t.fs_db = g.first.first;
    t.fs_name = g.first.second;
    decode_fs_name(t.fs_db, &t.db);
    decode_fs_name(t.fs_name, &t.name);

    std::vector<Space_ref> &spaces = g.second;
    std::sort(
        spaces.begin(), spaces.end(),
        [](const Space_ref &a, const Space_ref &b) { return a.path < b.path; });

    /* The definition is in the SDI of the table's only tablespace, or of the
    first partition. */
    std::unique_ptr<dd::Table> def;
    for (const Space_ref &s : spaces) {
      std::vector<prepare::Sdi_table> sdi;
      if (prepare::get_sdi_tables(s.id, &sdi) != DB_SUCCESS) {
        continue;
      }
      for (auto &e : sdi) {
        if (e.table != nullptr &&
            same_name(t.name, std::string(e.table->name().c_str(),
                                          e.table->name().length()))) {
          t.sdi_id = e.sdi_id;
          def = std::move(e.table);
          break;
        }
      }
      if (def != nullptr) break;
    }

    if (def == nullptr) {
      t.collect_error = "cannot read the table definition from the backup";
    } else {
      const dd::Table &table_def = *def;
      describe_table(table_def, &t);
      const size_t n_parts = table_def.leaf_partitions().size();
      if (t.partitioned && n_parts != spaces.size()) {
        t.collect_error = "the backup has " + std::to_string(spaces.size()) +
                          " of the " + std::to_string(n_parts) +
                          " partition files";
      }
    }

    for (const Space_ref &s : spaces) {
      Table_file f;
      f.space_name = s.name;
      f.backup_path = s.path;
      const std::string file = s.path.substr(s.path.find('/') + 1);

      const std::string external =
          Tablespace_map::instance().external_file_name(s.name);
      if (!external.empty()) {
        f.target_path = external;
        f.import_path = external;
        /* DATA DIRECTORY is the directory holding <db>/<table>.ibd. */
        const std::string db_dir = external.substr(0, external.rfind('/'));
        t.data_dir = db_dir.substr(0, db_dir.rfind('/'));
      } else {
        f.target_path = g_server.datadir + "/" + t.fs_db + "/" + file;
        f.import_path = t.fs_db + "/" + file;
      }

      fil_space_t *space = fil_space_get(s.id);
      if (space != nullptr && FSP_FLAGS_GET_ENCRYPTION(space->flags)) {
        t.encrypted = true;
        f.cfp_tmp_path = g_tmpdir + "/" + std::to_string(++cfp_count) + ".cfp";
        f.cfp_target_path =
            f.target_path.substr(0, f.target_path.size() - 4) + ".cfp";
        if (!xb_write_cfp_file(space, f.cfp_tmp_path.c_str())) {
          t.collect_error = "cannot write the encryption data of " + s.path;
        }
      }
      t.files.push_back(f);
    }

    /* The AUTO_INCREMENT counter, as the backup's dictionary has it after
    recovery; for a partitioned table, the largest of its partitions. */
    if (t.has_autoinc && t.collect_error.empty()) {
      for (const Space_ref &s : spaces) {
        dberr_t err;
        std::vector<dict_table_t *> tables;
        std::tie(err, tables) = prepare::dict_load_from_spaces_sdi(s.id);
        if (err != DB_SUCCESS) continue;
        mutex_enter(&dict_sys->mutex);
        for (dict_table_t *table : tables) {
          t.autoinc = std::max<uint64_t>(t.autoinc, table->autoinc);
          dd_table_close(table, thd, nullptr, true);
        }
        mutex_exit(&dict_sys->mutex);
      }
    }

    for (const auto &row : trigger_rows) {
      if (row.table_id == t.sdi_id && t.sdi_id != 0) {
        t.triggers.push_back(row);
      }
    }
    std::sort(t.triggers.begin(), t.triggers.end(),
              [](const prepare::Trigger_row &a, const prepare::Trigger_row &b) {
                return std::tie(a.action_timing, a.event_type, a.action_order) <
                       std::tie(b.action_timing, b.event_type, b.action_order);
              });

    if (g_schemas.count(t.db) == 0) {
      Schema schema;
      schema.name = t.db;
      for (const auto &row : schema_rows) {
        if (row.name == t.db) {
          collation_names(row.collation_id, &schema.collation, &schema.charset);
          schema.default_encryption = row.default_encryption;
        }
      }
      g_schemas[t.db] = schema;
    }

    g_tables.push_back(std::move(t));
  }

  xb::info() << "--restore: " << g_tables.size()
             << " table(s) selected from the backup";
  return true;
}

/** Copy a file, creating the target, which must not exist.
@param[in]	from	source
@param[in]	to	target
@param[in]	uid	owner to give the target
@param[in]	gid	group to give the target
@param[out]	error	why it failed
@return true on success */
static bool copy_file(const std::string &from, const std::string &to, uid_t uid,
                      gid_t gid, std::string *error) {
  const int in = open(from.c_str(), O_RDONLY);
  if (in < 0) {
    *error = "cannot open " + from + ": " + strerror(errno);
    return false;
  }
  const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0640);
  if (out < 0) {
    *error = "cannot create " + to + ": " + strerror(errno);
    close(in);
    return false;
  }

  bool ok = true;
  std::vector<char> buf(4 * 1024 * 1024);
  for (;;) {
    const ssize_t n = read(in, buf.data(), buf.size());
    if (n < 0) {
      *error = "cannot read " + from + ": " + strerror(errno);
      ok = false;
      break;
    }
    if (n == 0) break;
    ssize_t done = 0;
    while (done < n) {
      const ssize_t w = write(out, buf.data() + done, n - done);
      if (w < 0) {
        *error = "cannot write " + to + ": " + strerror(errno);
        ok = false;
        break;
      }
      done += w;
    }
    if (!ok) break;
  }

  /* The server reads the file as its own OS user, so it gets the owner of the
  directory it goes into. */
  if (ok && (fchown(out, uid, gid) != 0 || fchmod(out, 0640) != 0)) {
    *error = "cannot give " + to + " the owner of its directory (" +
             strerror(errno) +
             "); run --restore as root or as the server's OS user";
    ok = false;
  }
  if (ok && fsync(out) != 0) {
    *error = "cannot sync " + to + ": " + strerror(errno);
    ok = false;
  }
  close(out);
  close(in);
  if (!ok) {
    unlink(to.c_str());
  }
  return ok;
}

/** Whether DATA DIRECTORY of a table is one the server accepts, by the rule
CREATE TABLE ... DATA DIRECTORY applies.
@param[in]	t	table
@param[out]	error	why not
@return true if accepted */
static bool check_data_dir(const Table &t, std::string *error) {
  const std::string dir = resolve_path(t.data_dir + "/" + t.fs_db);
  if (dir == g_server.datadir) {
    *error = "its DATA DIRECTORY is the datadir";
    return false;
  }
  if (is_same_or_under(g_server.unique_undo_dir, dir)) {
    *error = "its DATA DIRECTORY is in the undo directory";
    return false;
  }
  for (const auto &known : g_server.known_dirs) {
    if (is_same_or_under(known, dir)) {
      return true;
    }
  }
  *error = "its DATA DIRECTORY " + t.data_dir +
           " is not in a directory InnoDB knows; add it to "
           "innodb_directories and restart the server";
  return false;
}

/** Remove the files of a table that the restore placed and that are still
there.
@param[in]	t	table */
static void remove_placed_files(const Table &t) {
  for (const auto &f : t.files) {
    unlink(f.target_path.c_str());
    if (!f.cfp_target_path.empty()) {
      unlink(f.cfp_target_path.c_str());
    }
  }
}

/** Create a trigger the way the backup describes it.
@param[in]	conn	connection
@param[in]	t	table
@param[in]	tr	trigger
@param[out]	error	the server's error
@return true on success */
static bool create_trigger(MYSQL *conn, const Table &t,
                           const prepare::Trigger_row &tr, std::string *error) {
  static const char *timings[] = {"", "BEFORE", "AFTER"};
  static const char *events[] = {"", "INSERT", "UPDATE", "DELETE"};
  if (tr.action_timing < 1 || tr.action_timing > 2 || tr.event_type < 1 ||
      tr.event_type > 3) {
    *error = "unknown timing or event in the backup";
    return false;
  }

  const size_t at = tr.definer.rfind('@');
  const std::string user =
      at == std::string::npos ? tr.definer : tr.definer.substr(0, at);
  const std::string host =
      at == std::string::npos ? std::string("%") : tr.definer.substr(at + 1);

  std::string client_coll, client_cs, conn_coll, conn_cs;
  collation_names(tr.client_collation_id, &client_coll, &client_cs);
  collation_names(tr.connection_collation_id, &conn_coll, &conn_cs);

  /* The body is in the client character set the trigger was created with.
  Send it with that character set, unless it is neither UTF-8 nor able to
  carry the non-ASCII names of the statement; then use the UTF-8 copy. */
  const bool utf8_client =
      client_cs.compare(0, 4, "utf8") == 0 || client_cs.empty();
  const std::string names = t.db + t.name + tr.name + user + host;
  const bool ascii_names = std::all_of(names.begin(), names.end(), [](char c) {
    return static_cast<unsigned char>(c) < 0x80;
  });
  const bool original = utf8_client || ascii_names;

  std::vector<std::string> setup;
  setup.push_back("SET SESSION sql_mode = " + std::to_string(tr.sql_mode));
  if (original && !client_cs.empty()) {
    setup.push_back("SET SESSION character_set_client = " + client_cs);
  }
  if (!conn_coll.empty()) {
    setup.push_back("SET SESSION collation_connection = " + conn_coll);
  }

  bool ok = true;
  for (const auto &q : setup) {
    if (!exec(conn, q, error)) {
      ok = false;
      break;
    }
  }

  if (ok) {
    const std::string stmt =
        "CREATE DEFINER=" + quote_ident(user) + "@" + quote_ident(host) +
        " TRIGGER " + quote_ident(t.db) + "." + quote_ident(tr.name) + " " +
        timings[tr.action_timing] + " " + events[tr.event_type] + " ON " +
        qualified(t) + " FOR EACH ROW " +
        (original ? tr.action_statement : tr.action_statement_utf8);
    ok = exec(conn, stmt, error);
  }

  std::string ignored;
  exec(conn, "SET NAMES utf8mb4", &ignored);
  exec(conn, "SET SESSION sql_mode = DEFAULT", &ignored);
  return ok;
}

/** Restore one table.
@param[in]	conn	connection
@param[in,out]	t	table */
static void restore_table(MYSQL *conn, Table *t) {
  std::string error;

  if (!t->collect_error.empty()) {
    t->reason = t->collect_error;
    return;
  }
  const auto schema = g_schemas.find(t->db);
  if (schema != g_schemas.end() && !schema->second.error.empty()) {
    t->reason = schema->second.error;
    return;
  }

  /* An existing table is never replaced. The import fails as well when one
  appears in the meantime; this check only saves the copy. */
  std::vector<std::vector<std::string>> rows;
  if (!select_rows(conn,
                   "SELECT 1 FROM information_schema.TABLES WHERE "
                   "TABLE_SCHEMA = " +
                       quote_string(conn, t->db) +
                       " AND TABLE_NAME = " + quote_string(conn, t->name),
                   &rows, &error)) {
    t->reason = "cannot check whether the table exists: " + error;
    return;
  }
  if (!rows.empty()) {
    t->reason = "the table already exists";
    return;
  }

  if (!t->data_dir.empty() && !check_data_dir(*t, &error)) {
    t->reason = error;
    return;
  }

  for (const auto &f : t->files) {
    struct stat st;
    if (stat(f.target_path.c_str(), &st) == 0) {
      t->reason = "a file is already at " + f.target_path;
      return;
    }
  }

  /* The files get the owner of the database directory in the datadir. */
  struct stat db_dir;
  const std::string db_path = g_server.datadir + "/" + t->fs_db;
  if (stat(db_path.c_str(), &db_dir) != 0) {
    t->reason = "cannot read " + db_path + ": " + strerror(errno);
    return;
  }

  for (const auto &f : t->files) {
    const std::string dir = f.target_path.substr(0, f.target_path.rfind('/'));
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
      if (mkdir(dir.c_str(), 0750) != 0 ||
          chown(dir.c_str(), db_dir.st_uid, db_dir.st_gid) != 0) {
        t->reason = "cannot create " + dir + ": " + strerror(errno);
        remove_placed_files(*t);
        return;
      }
    }
    if (!copy_file(f.backup_path, f.target_path, db_dir.st_uid, db_dir.st_gid,
                   &error) ||
        (!f.cfp_tmp_path.empty() &&
         !copy_file(f.cfp_tmp_path, f.cfp_target_path, db_dir.st_uid,
                    db_dir.st_gid, &error))) {
      t->reason = error;
      remove_placed_files(*t);
      return;
    }
  }

  /* IMPORT TABLE resolves a relative path against the datadir, but refuses to
run without a current database. */
  if (!exec(conn, "USE " + quote_ident(t->db), &error)) {
    t->reason = "cannot use database " + t->db + ": " + error;
    remove_placed_files(*t);
    return;
  }

  std::string stmt = "IMPORT TABLE FROM ";
  for (size_t i = 0; i < t->files.size(); ++i) {
    if (i > 0) stmt += ", ";
    stmt += quote_string(conn, escape_glob(t->files[i].import_path));
  }

  xb::info() << "--restore: importing " << t->db << "." << t->name;
  const bool imported = exec(conn, stmt, &error);

  /* The server leaves the .cfp files; they carry the tablespace key. On
  failure it removes the files it converted, and the rest goes too. */
  if (!imported) {
    /* When the connection broke, the import may have committed without the
    answer reaching us: the files may belong to a table now, so they stay and
    the outcome is reported as unknown. Any answer from the server is final:
    it removed what it converted, and the rest goes too. */
    const unsigned int err_no = mysql_errno(conn);
    if (err_no == CR_SERVER_LOST || err_no == CR_SERVER_GONE_ERROR) {
      t->reason = "the connection broke during IMPORT TABLE (" + error +
                  "); check whether the table exists. Its files are left "
                  "in place";
      return;
    }
    t->reason = "IMPORT TABLE failed: " + error;
    remove_placed_files(*t);
    return;
  }
  for (const auto &f : t->files) {
    if (!f.cfp_target_path.empty()) {
      unlink(f.cfp_target_path.c_str());
    }
  }
  t->restored = true;

  if (t->has_autoinc && t->autoinc > 0 &&
      !exec(conn,
            "ALTER TABLE " + qualified(*t) +
                " AUTO_INCREMENT = " + std::to_string(t->autoinc),
            &error)) {
    t->warnings.push_back("AUTO_INCREMENT not set to " +
                          std::to_string(t->autoinc) + ": " + error);
  }

  for (const auto &fts : t->fts) {
    std::string cols;
    for (const auto &c : fts.columns) {
      if (!cols.empty()) cols += ", ";
      cols += quote_ident(c);
    }
    /* The import leaves the index unusable. Dropping it and adding it in
    one ALTER TABLE keeps it so, because the definition does not change;
    two statements rebuild it. */
    std::string add = "ALTER TABLE " + qualified(*t) + " ADD FULLTEXT INDEX " +
                      quote_ident(fts.name) + " (" + cols + ")";
    if (!fts.parser.empty()) {
      add += " WITH PARSER " + quote_ident(fts.parser);
    }
    if (!exec(conn,
              "ALTER TABLE " + qualified(*t) + " DROP INDEX " +
                  quote_ident(fts.name),
              &error) ||
        !exec(conn, add, &error)) {
      t->warnings.push_back("FULLTEXT index " + fts.name +
                            " not rebuilt: " + error);
    }
  }

  for (const auto &tr : t->triggers) {
    if (!create_trigger(conn, *t, tr, &error)) {
      t->warnings.push_back("trigger " + tr.name + " not created: " + error);
    }
  }
}

/** Create the databases of the selected tables that the server lacks, with
the defaults they have in the backup.
@param[in]	conn	connection */
static void create_schemas(MYSQL *conn) {
  for (auto &s : g_schemas) {
    Schema &schema = s.second;
    std::string error;
    std::vector<std::vector<std::string>> rows;
    if (!select_rows(conn,
                     "SELECT 1 FROM information_schema.SCHEMATA WHERE "
                     "SCHEMA_NAME = " +
                         quote_string(conn, schema.name),
                     &rows, &error)) {
      schema.error = "cannot check whether the database exists: " + error;
      continue;
    }
    if (!rows.empty()) {
      continue;
    }
    std::string q = "CREATE DATABASE " + quote_ident(schema.name);
    if (!schema.charset.empty()) {
      q += " CHARACTER SET " + schema.charset + " COLLATE " + schema.collation;
    }
    q += schema.default_encryption ? " DEFAULT ENCRYPTION = 'Y'"
                                   : " DEFAULT ENCRYPTION = 'N'";
    xb::info() << "--restore: " << q;
    if (!exec(conn, q, &error)) {
      schema.error = "cannot create the database: " + error;
    }
  }
}

/** After every table is in, warn about foreign keys whose parent table does
not exist.
@param[in]	conn	connection */
static void check_foreign_keys(MYSQL *conn) {
  for (auto &t : g_tables) {
    if (!t.restored) continue;
    std::string error;
    std::vector<std::vector<std::string>> rows;
    if (!select_rows(
            conn,
            "SELECT rc.CONSTRAINT_NAME, rc.UNIQUE_CONSTRAINT_SCHEMA, "
            "rc.REFERENCED_TABLE_NAME FROM "
            "information_schema.REFERENTIAL_CONSTRAINTS rc WHERE "
            "rc.CONSTRAINT_SCHEMA = " +
                quote_string(conn, t.db) +
                " AND rc.TABLE_NAME = " + quote_string(conn, t.name) +
                " AND NOT EXISTS (SELECT 1 FROM information_schema.TABLES tb "
                "WHERE tb.TABLE_SCHEMA = rc.UNIQUE_CONSTRAINT_SCHEMA AND "
                "tb.TABLE_NAME = rc.REFERENCED_TABLE_NAME)",
            &rows, &error)) {
      continue;
    }
    for (const auto &r : rows) {
      t.warnings.push_back("foreign key " + r[0] + " references " + r[1] + "." +
                           r[2] + ", which does not exist");
    }
  }
}

/** Write the --restore-report JSON file.
@param[in]	path	file */
static void write_report(const char *path) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  size_t restored = 0, failed = 0, warned = 0;

  w.StartObject();
  w.Key("tables");
  w.StartArray();
  for (const auto &t : g_tables) {
    w.StartObject();
    w.Key("database");
    w.String(t.db.c_str());
    w.Key("table");
    w.String(t.name.c_str());
    w.Key("status");
    w.String(t.restored ? "restored" : "failed");
    if (!t.restored) {
      w.Key("reason");
      w.String(t.reason.c_str());
    }
    w.Key("warnings");
    w.StartArray();
    for (const auto &warning : t.warnings) {
      w.String(warning.c_str());
    }
    w.EndArray();
    w.EndObject();
    if (t.restored) {
      ++restored;
      if (!t.warnings.empty()) ++warned;
    } else {
      ++failed;
    }
  }
  w.EndArray();
  w.Key("restored");
  w.Uint64(restored);
  w.Key("failed");
  w.Uint64(failed);
  w.Key("with_warnings");
  w.Uint64(warned);
  w.EndObject();

  std::ofstream out(path);
  out << buf.GetString() << "\n";
  if (!out) {
    xb::error() << "--restore: cannot write the report to " << path;
  }
}

/** Remove the temporary .cfp directory. */
static void remove_tmpdir() {
  if (g_tmpdir.empty()) return;
  for (const auto &t : g_tables) {
    for (const auto &f : t.files) {
      if (!f.cfp_tmp_path.empty()) unlink(f.cfp_tmp_path.c_str());
    }
  }
  rmdir(g_tmpdir.c_str());
}

int run() {
  if (opt_restore_dry_run) {
    xb::info() << "--restore --dry-run: these tables would be restored:";
    for (const auto &t : g_tables) {
      std::ostringstream s;
      s << t.db << "." << t.name << ": " << t.files.size() << " file(s)";
      if (t.partitioned) s << ", partitioned";
      if (t.encrypted) s << ", encrypted";
      if (!t.data_dir.empty()) s << ", DATA DIRECTORY " << t.data_dir;
      if (t.has_autoinc) s << ", AUTO_INCREMENT " << t.autoinc;
      if (!t.fts.empty()) s << ", " << t.fts.size() << " FULLTEXT index(es)";
      if (!t.triggers.empty()) s << ", " << t.triggers.size() << " trigger(s)";
      if (!t.collect_error.empty())
        s << "; cannot restore: " << t.collect_error;
      xb::info() << "  " << s.str();
    }
    remove_tmpdir();
    return RESTORE_EXIT_OK;
  }

  Connection main;
  if (!open_restore_connection(main)) {
    remove_tmpdir();
    return RESTORE_EXIT_FATAL;
  }
  create_schemas(main);

  /* Workers take the tables one at a time, each on its own connection. */
  std::atomic<size_t> next{0};
  std::atomic<bool> connect_failed{false};
  auto worker = [&]() {
    Connection conn;
    if (!open_restore_connection(conn)) {
      connect_failed = true;
      return;
    }
    size_t i;
    while ((i = next++) < g_tables.size()) {
      restore_table(conn, &g_tables[i]);
      if (g_tables[i].restored) {
        xb::info() << "--restore: restored " << g_tables[i].db << "."
                   << g_tables[i].name;
      } else {
        xb::error() << "--restore: " << g_tables[i].db << "."
                    << g_tables[i].name
                    << " not restored: " << g_tables[i].reason;
      }
    }
  };

  const int n = std::max(
      1, std::min<int>(xtrabackup_parallel, static_cast<int>(g_tables.size())));
  std::vector<std::thread> threads;
  for (int i = 0; i < n; ++i) {
    threads.emplace_back(worker);
  }
  for (auto &th : threads) {
    th.join();
  }

  for (auto &t : g_tables) {
    if (!t.restored && t.reason.empty()) {
      t.reason =
          connect_failed ? "cannot connect to the server" : "not attempted";
    }
  }

  check_foreign_keys(main);

  size_t restored = 0, failed = 0, warned = 0;
  xb::info() << "--restore summary:";
  for (const auto &t : g_tables) {
    if (t.restored) {
      ++restored;
      xb::info() << "  RESTORED " << t.db << "." << t.name;
      if (!t.warnings.empty()) ++warned;
    } else {
      ++failed;
      xb::info() << "  FAILED   " << t.db << "." << t.name << ": " << t.reason;
    }
    for (const auto &warning : t.warnings) {
      xb::info() << "  WARNING  " << t.db << "." << t.name << ": " << warning;
    }
  }
  xb::info() << "--restore: " << restored << " of " << g_tables.size()
             << " table(s) restored, " << failed << " failed, " << warned
             << " with warnings";

  if (opt_restore_report != nullptr) {
    write_report(opt_restore_report);
  }
  remove_tmpdir();

  return (failed == 0 && warned == 0) ? RESTORE_EXIT_OK : RESTORE_EXIT_PARTIAL;
}

}  // namespace restore
}  // namespace xb
