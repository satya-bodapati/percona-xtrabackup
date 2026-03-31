/*****************************************************************************

Copyright (c) 2016, 2023, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#include "my_config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <limits>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <zlib.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "my_rapidjson_size_t.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <exception>
#include <iostream>
#include <sstream>
#include "m_ctype.h"
#include "m_string.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_sys.h"
#include "print_version.h"
#include "typelib.h"
#include "welcome_copyright_notice.h"

#include "btr0cur.h"
#include "dict0sdi-decompress.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "lob0lob.h"
#include "mach0data.h"
#include "page0page.h"
#include "page0size.h"
#include "page0types.h"
#include "univ.i"
#include "ut0byte.h"
#include "ut0crc32.h"

typedef enum { SUCCESS, FALIURE, NO_RECORDS } err_t;

/** Length of ID field in record of SDI Index. */
static const uint32_t REC_DATA_ID_LEN = 8;

/** Length of TYPE field in record of SDI Index. */
static const uint32_t REC_DATA_TYPE_LEN = 4;

/** Length of UNCOMPRESSED_LEN field in record of SDI Index. */
static const uint32_t REC_DATA_UNCOMP_LEN = 4;

/** Length of COMPRESSED_LEN field in record of SDI Index. */
static const uint32_t REC_DATA_COMP_LEN = 4;

/** SDI Index record Origin. */
static const uint32_t REC_ORIGIN = 0;

/** Length of SDI Index record header. */
static const uint32_t REC_MIN_HEADER_SIZE = REC_N_NEW_EXTRA_BYTES;

/** Stored at rec origin minus 3rd byte. Only 3bits of 3rd byte are used for
rec type. */
static const uint32_t REC_OFF_TYPE = 3;

/** Stored at rec_origin minus 2nd byte and length 2 bytes. */
static const uint32_t REC_OFF_NEXT = 2;

/** Offset of TYPE field in record (0). */
static const uint32_t REC_OFF_DATA_TYPE = REC_ORIGIN;
// ut_ad(REC_OFF_DATA_ID == 0);

/** Offset of ID field in record (4). */
static const uint32_t REC_OFF_DATA_ID = REC_OFF_DATA_TYPE + REC_DATA_TYPE_LEN;
// ut_ad(REC_OFF_DATA_TYPE == 8);

/** Offset of 6-byte trx id (12). */
static const uint32_t REC_OFF_DATA_TRX_ID = REC_OFF_DATA_ID + REC_DATA_ID_LEN;
// ut_ad(REC_OFF_DATA_TRX_ID == 12);

/** 7-byte roll-ptr (18). */
static const uint32_t REC_OFF_DATA_ROLL_PTR =
    REC_OFF_DATA_TRX_ID + DATA_TRX_ID_LEN;
// ut_ad(REC_OFF_DATA_ROLL_PTR == 18);

/** 4-byte un-compressed len (25) */
static const uint32_t REC_OFF_DATA_UNCOMP_LEN =
    REC_OFF_DATA_ROLL_PTR + DATA_ROLL_PTR_LEN;

/** 4-byte compressed len (29) */
static const uint32_t REC_OFF_DATA_COMP_LEN =
    REC_OFF_DATA_UNCOMP_LEN + REC_DATA_UNCOMP_LEN;

/** Variable length Data (33). */
static const uint32_t REC_OFF_DATA_VARCHAR =
    REC_OFF_DATA_COMP_LEN + REC_DATA_COMP_LEN;

// ut_ad(REC_OFF_DATA_VARCHAR == 33);

/** Record size in page. This will be used determine the maximum number
of records on a page. */
static const uint32_t SDI_REC_SIZE = 1                     /* rec_len */
                                     + REC_MIN_HEADER_SIZE /* rec_header */
                                     + REC_DATA_TYPE_LEN   /* type field size */
                                     + REC_DATA_ID_LEN     /* id field len */
                                     + DATA_ROLL_PTR_LEN   /* roll ptr len */
                                     + DATA_TRX_ID_LEN /* TRX_ID len */;

/** If page 0 is corrupted, the maximum number of pages to scan to
determine page size. */
static const page_no_t MAX_PAGES_TO_SCAN = 60;

/** Indicates error. */
static const uint64_t IB_ERROR = SIZE_T_MAX;
static const uint32_t IB_ERROR_32 = (~((uint32)0));

/** SDI BLOB not expected before the following page number.
0 (tablespace header), 1 (tabespace bitmap), 2 (ibuf bitmap)
3 (SDI Index root page) */
static const uint64_t SDI_BLOB_ALLOWED = 4;

/** Replaces declaration in srv0srv.c */
ulong srv_page_size;
ulong srv_page_size_shift;
page_size_t univ_page_size(0, 0, false);

/** Global options structure. Option values passed at command line are
stored in this structure */
struct sdi_options {
  uint64_t sdi_rec_id;
  uint64_t sdi_rec_type;
  bool skip_data;
  bool is_sdi_id;
  bool is_sdi_type;
  bool is_sdi_rec;
  bool no_checksum;
  bool is_dump_file;
  const char *dbug_setting;
  char *dump_filename;
  ulong strict_check;
  bool pretty;
  bool create_sql;
};
struct sdi_options opts;

/** Possible values for "--strict-check" for strictly verify checksum */
static const char *checksum_algorithms[] = {"crc32", "innodb", "none", NullS};

/** Used to define an enumerate type of the "checksum algorithm". */
static TYPELIB checksum_algorithms_typelib = {
    array_elements(checksum_algorithms) - 1, "", checksum_algorithms, nullptr};

/* Command line argument for ibd2sdi tool. */
static struct my_option ibd2sdi_options[] = {
    {"help", 'h', "Display this help and exit.", nullptr, nullptr, nullptr,
     GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"version", 'v', "Display version information and exit.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
#ifndef NDEBUG
    {"debug", '#', "Output debug log. See " REFMAN "dbug-package.html",
     &opts.dbug_setting, &opts.dbug_setting, nullptr, GET_STR, OPT_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
#endif /* !NDEBUG */
    {"dump-file", 'd',
     "Dump the tablespace SDI into the file passed by user."
     " Without the filename, it will default to stdout",
     &opts.dump_filename, &opts.dump_filename, nullptr, GET_STR, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
    {"skip-data", 's',
     "Skip retrieving data from SDI records. Retrieve only"
     " id and type.",
     &opts.skip_data, &opts.skip_data, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"id", 'i', "Retrieve the SDI record matching the id passed by user.",
     &opts.sdi_rec_id, &opts.sdi_rec_id, nullptr, GET_ULL, REQUIRED_ARG, 0, 0,
     ULLONG_MAX, nullptr, 1, nullptr},
    {"type", 't', "Retrieve the SDI records matching the type passed by user.",
     &opts.sdi_rec_type, &opts.sdi_rec_type, nullptr, GET_ULL, REQUIRED_ARG, 0,
     0, ULLONG_MAX, nullptr, 1, nullptr},
    {"strict-check", 'c',
     "Specify the strict checksum algorithm by the user."
     " Allowed values are innodb, crc32, none.",
     &opts.strict_check, &opts.strict_check, &checksum_algorithms_typelib,
     GET_ENUM, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"no-check", 'n', "Ignore the checksum verification.", &opts.no_checksum,
     &opts.no_checksum, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"pretty", 'p',
     "Pretty format the SDI output."
     "If false, SDI would be not human readable but it will be of less size",
     &opts.pretty, &opts.pretty, nullptr, GET_BOOL, OPT_ARG, 1, 0, 0, nullptr,
     0, nullptr},
    {"create-sql", 'S',
     "Generate CREATE TABLE SQL from SDI data instead of dumping JSON."
     " Output is written to stdout and to a .sql file alongside the .ibd file.",
     &opts.create_sql, &opts.create_sql, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},

    {nullptr, 0, nullptr, nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0,
     0, nullptr, 0, nullptr}};

/** A dummy implementation.  Actual implementation available in fil0fil.cc */
std::ostream &Fil_page_header::print(std::ostream &out) const noexcept {
  return out;
}

/** Report a failed assertion.
@param[in]	expr	the failed assertion if not NULL
@param[in]	file	source file containing the assertion
@param[in]	line	line number of the assertion */
[[noreturn]] void ut_dbg_assertion_failed(const char *expr, const char *file,
                                          uint64_t line) {
  fprintf(stderr, "ibd2sdi: Assertion failure in file %s line " UINT64PF "\n",
          file, line);

  if (expr != nullptr) {
    fprintf(stderr, "ibd2sdi: Failing assertion: %s\n", expr);
  }

  fflush(stderr);
  fflush(stdout);
  abort();
}

/** Tries to delete temporary file.
@param[in]	temp_filename	The name of file to delete*/
static void try_delete_temporary_filename(const char *temp_filename) {
  if (my_delete(temp_filename, MYF(0)) != 0) {
    ib::warn() << "Removal of temporary file " << temp_filename
               << " failed because of system error: " << strerror(errno);
  }
}

/** Create a file in a system's temporary directory.
@param[in,out]	temp_file_buf	Buffer to hold the temporary file
                                name generated
@param[in]	dir		directory used for creation of
                                temporary file, nullptr if system
                                tmpdir to be used
@param[in]	prefix_pattern	the temp file name is prefixed with
                                this string
@return File pointer of the created file */
static FILE *create_tmp_file(char *temp_file_buf, const char *dir,
                             const char *prefix_pattern) {
  FILE *file = nullptr;
  File fd = create_temp_file(temp_file_buf, dir, prefix_pattern,
                             O_CREAT | O_RDWR, KEEP_FILE, MYF(0));

  if (fd >= 0) {
    file = my_fdopen(fd, temp_file_buf, O_RDWR, MYF(0));
  }

  DBUG_EXECUTE_IF("ib_tmp_file_fail", file = nullptr; errno = EACCES;);

  if (file == nullptr) {
    ib::error() << "Unable to create temporary file. err: " << strerror(errno);

    if (fd >= 0) {
      try_delete_temporary_filename(temp_file_buf);
      my_close(fd, MYF(0));
    }
  }

  return (file);
}

/** Print the ibd2sdi tool usage. */
static void usage() {
#ifdef NDEBUG
  print_version();
#else
  print_version_debug();
#endif /* NDEBUG */
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2015"));
  printf(
      "Usage: %s [-v] [-c <strict-check>] [-d <dump file name>] [-n]"
      " filename1 [filenames]\n",
      my_progname);
  printf("See " REFMAN "ibd2sdi.html for usage hints.\n");
  my_print_help(ibd2sdi_options);
  my_print_variables(ibd2sdi_options);
}

/** Parse the options passed to tool. */
extern "C" bool ibd2sdi_get_one_option(int optid,
                                       const struct my_option *opt
                                       [[maybe_unused]],
                                       char *argument [[maybe_unused]]) {
  switch (optid) {
#ifndef NDEBUG
    case '#':
      opts.dbug_setting =
          argument ? argument
                   : IF_WIN("d:O,ibd2sdi.trace", "d:o,/tmp/ibd2sdi.trace");
      DBUG_PUSH(opts.dbug_setting);
      break;
#endif /* !NDEBUG */
    case 'v':
#ifdef NDEBUG
      print_version();
#else
      print_version_debug();
#endif /* DBUG */
      exit(EXIT_SUCCESS);
      break;
    case 'c':
      switch (opts.strict_check) {
        case 0:
          srv_checksum_algorithm = SRV_CHECKSUM_ALGORITHM_STRICT_CRC32;
          break;
        case 1:
          srv_checksum_algorithm = SRV_CHECKSUM_ALGORITHM_STRICT_INNODB;
          break;

        case 2:
          srv_checksum_algorithm = SRV_CHECKSUM_ALGORITHM_STRICT_NONE;
          break;
        default:
          return (true);
      }
      break;
    case 'd':
      opts.is_dump_file = true;
      break;
    case 'i':
      opts.is_sdi_id = true;
      break;
    case 't':
      opts.is_sdi_type = true;
      break;
    case 'n':
      opts.no_checksum = true;
      break;
    case 'p':
      break;
    case 'S':
      opts.create_sql = true;
      break;
    case 'h':
      usage();
      exit(EXIT_SUCCESS);
      break;
  }

  return (false);
}

/** Retrieve the options passed to the tool. */
static bool get_options(int *argc, char ***argv) {
  if (handle_options(argc, argv, ibd2sdi_options, ibd2sdi_get_one_option)) {
    exit(true);
  }

  /* The next arg must be the filename */
  if (!*argc) {
    usage();
    return (true);
  }

  return (false);
}

/* ========================================================================
   --create-sql: Generate CREATE TABLE SQL from SDI JSON
   ======================================================================== */
namespace create_sql {

namespace col_type {
constexpr int DECIMAL = 1;
constexpr int TINY = 2;
constexpr int SHORT = 3;
constexpr int LONG = 4;
constexpr int FLOAT = 5;
constexpr int DOUBLE = 6;
constexpr int TIMESTAMP = 8;
constexpr int LONGLONG = 9;
constexpr int INT24 = 10;
constexpr int VARCHAR = 16;
constexpr int BIT = 17;
constexpr int TIMESTAMP2 = 18;
constexpr int NEWDECIMAL = 21;
constexpr int TINY_BLOB = 24;
constexpr int MEDIUM_BLOB = 25;
constexpr int LONG_BLOB = 26;
constexpr int BLOB = 27;
constexpr int VAR_STRING = 28;
constexpr int STRING = 29;
constexpr int GEOMETRY = 30;
constexpr int JSON = 31;
}  // namespace col_type

namespace col_hidden {
constexpr int HT_VISIBLE = 1;
constexpr int HT_HIDDEN_SE = 2;
constexpr int HT_HIDDEN_SQL = 3;
constexpr int HT_HIDDEN_USER = 4;
}  // namespace col_hidden

namespace idx_type {
constexpr int IT_PRIMARY = 1;
constexpr int IT_UNIQUE = 2;
constexpr int IT_MULTIPLE = 3;
constexpr int IT_FULLTEXT = 4;
constexpr int IT_SPATIAL = 5;
}  // namespace idx_type

namespace idx_algo {
constexpr int IA_BTREE = 2;
constexpr int IA_RTREE = 3;
constexpr int IA_HASH = 4;
}  // namespace idx_algo

namespace row_fmt {
constexpr int RF_COMPRESSED = 3;
constexpr int RF_REDUNDANT = 4;
constexpr int RF_COMPACT = 5;
}  // namespace row_fmt

namespace fk_rule {
constexpr int RULE_NO_ACTION = 1;
constexpr int RULE_RESTRICT = 2;
constexpr int RULE_CASCADE = 3;
constexpr int RULE_SET_NULL = 4;
constexpr int RULE_SET_DEFAULT = 5;
}  // namespace fk_rule

namespace idx_elem_order {
constexpr int ORDER_DESC = 3;
}  // namespace idx_elem_order

namespace chk_state {
constexpr int CS_NOT_ENFORCED = 1;
}  // namespace chk_state

namespace part_type {
constexpr int PT_NONE = 0;
constexpr int PT_HASH = 1;
constexpr int PT_KEY_51 = 2;
constexpr int PT_KEY_55 = 3;
constexpr int PT_LINEAR_HASH = 4;
constexpr int PT_LINEAR_KEY_51 = 5;
constexpr int PT_LINEAR_KEY_55 = 6;
constexpr int PT_RANGE = 7;
constexpr int PT_LIST = 8;
constexpr int PT_RANGE_COLUMNS = 9;
constexpr int PT_LIST_COLUMNS = 10;
}  // namespace part_type

namespace sub_type {
constexpr int ST_NONE = 0;
constexpr int ST_HASH = 1;
constexpr int ST_KEY_51 = 2;
constexpr int ST_KEY_55 = 3;
constexpr int ST_LINEAR_HASH = 4;
constexpr int ST_LINEAR_KEY_51 = 5;
constexpr int ST_LINEAR_KEY_55 = 6;
}  // namespace sub_type

static std::string quote_identifier(const std::string &name) {
  std::string r = "`";
  for (char c : name) {
    if (c == '`') r += '`';
    r += c;
  }
  r += "`";
  return r;
}

static std::string escape_string(const std::string &s) {
  std::string r;
  r.reserve(s.length());
  for (char c : s) {
    switch (c) {
      case '\0':
        r += "\\0";
        break;
      case '\n':
        r += "\\n";
        break;
      case '\r':
        r += "\\r";
        break;
      case '\\':
        r += "\\\\";
        break;
      case '\'':
        r += "''";
        break;
      default:
        r += c;
        break;
    }
  }
  return r;
}

static std::string unescape_dd_string(const std::string &s) {
  std::string result;
  result.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '\\' && i + 1 < s.length()) {
      result += s[i + 1];
      i++;
    } else {
      result += s[i];
    }
  }
  return result;
}

static std::map<std::string, std::string> parse_options_string(
    const std::string &raw) {
  std::map<std::string, std::string> result;
  std::string key, val;
  bool in_key = true;
  bool escaped = false;
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (escaped) {
      if (in_key)
        key += c;
      else
        val += c;
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '=' && in_key) {
      in_key = false;
      continue;
    }
    if (c == ';') {
      if (!key.empty()) result[key] = val;
      key.clear();
      val.clear();
      in_key = true;
      continue;
    }
    if (in_key)
      key += c;
    else
      val += c;
  }
  if (!key.empty()) result[key] = val;
  return result;
}

static std::string get_json_string(const rapidjson::Value &v,
                                   const char *key) {
  if (v.HasMember(key) && v[key].IsString()) return v[key].GetString();
  return "";
}

static int get_json_int(const rapidjson::Value &v, const char *key,
                        int def = 0) {
  if (v.HasMember(key) && v[key].IsInt()) return v[key].GetInt();
  return def;
}

static uint64_t get_json_uint64(const rapidjson::Value &v, const char *key,
                                uint64_t def = 0) {
  if (v.HasMember(key) && v[key].IsUint64()) return v[key].GetUint64();
  if (v.HasMember(key) && v[key].IsInt64())
    return static_cast<uint64_t>(v[key].GetInt64());
  return def;
}

static bool get_json_bool(const rapidjson::Value &v, const char *key,
                          bool def = false) {
  if (v.HasMember(key) && v[key].IsBool()) return v[key].GetBool();
  return def;
}

static bool is_numeric_type(int ct) {
  return ct == col_type::DECIMAL || ct == col_type::NEWDECIMAL ||
         ct == col_type::TINY || ct == col_type::SHORT ||
         ct == col_type::LONG || ct == col_type::LONGLONG ||
         ct == col_type::INT24 || ct == col_type::FLOAT ||
         ct == col_type::DOUBLE || ct == col_type::BIT;
}

static bool is_string_type(int ct) {
  return ct == col_type::VARCHAR || ct == col_type::VAR_STRING ||
         ct == col_type::STRING || ct == col_type::TINY_BLOB ||
         ct == col_type::MEDIUM_BLOB || ct == col_type::LONG_BLOB ||
         ct == col_type::BLOB;
}

static const char *fk_rule_str(int rule) {
  switch (rule) {
    case fk_rule::RULE_RESTRICT:
      return "RESTRICT";
    case fk_rule::RULE_CASCADE:
      return "CASCADE";
    case fk_rule::RULE_SET_NULL:
      return "SET NULL";
    case fk_rule::RULE_SET_DEFAULT:
      return "SET DEFAULT";
    default:
      return nullptr;
  }
}

static std::string generate_create_table_from_json(
    const rapidjson::Value &dd_obj) {
  std::ostringstream ss;

  std::string tbl_name = get_json_string(dd_obj, "name");
  ss << "CREATE TABLE " << quote_identifier(tbl_name) << " (\n";

  /* Build columns vector for column_opx resolution */
  struct ColInfo {
    std::string name;
    int hidden;
    int type;
    std::string column_type_utf8;
    std::string gen_expr_utf8;
    int collation_id;
  };
  std::vector<ColInfo> all_columns;

  const auto &columns = dd_obj["columns"];
  for (rapidjson::SizeType i = 0; i < columns.Size(); i++) {
    const auto &col = columns[i];
    ColInfo ci;
    ci.name = get_json_string(col, "name");
    ci.hidden = get_json_int(col, "hidden", col_hidden::HT_VISIBLE);
    ci.type = get_json_int(col, "type");
    ci.column_type_utf8 = get_json_string(col, "column_type_utf8");
    ci.gen_expr_utf8 = get_json_string(col, "generation_expression_utf8");
    ci.collation_id = get_json_int(col, "collation_id");
    all_columns.push_back(ci);
  }

  bool first_col = true;
  for (rapidjson::SizeType i = 0; i < columns.Size(); i++) {
    const auto &col = columns[i];
    int hidden = get_json_int(col, "hidden", col_hidden::HT_VISIBLE);
    if (hidden == col_hidden::HT_HIDDEN_SE ||
        hidden == col_hidden::HT_HIDDEN_SQL)
      continue;

    if (!first_col) ss << ",\n";
    first_col = false;

    std::string col_name = get_json_string(col, "name");
    std::string col_type_utf8 = get_json_string(col, "column_type_utf8");
    int col_type = get_json_int(col, "type");

    ss << "  " << quote_identifier(col_name) << " " << col_type_utf8;

    /* CHARACTER SET / COLLATE */
    if (get_json_bool(col, "is_explicit_collation")) {
      int coll_id = get_json_int(col, "collation_id");
      CHARSET_INFO *col_cs =
          get_charset(static_cast<uint>(coll_id), MYF(0));
      if (col_cs && strcmp(col_cs->csname, "binary") != 0 &&
          col_type != col_type::GEOMETRY) {
        ss << " CHARACTER SET " << col_cs->csname;
        ss << " COLLATE " << col_cs->m_coll_name;
      }
    }

    /* Generated column */
    std::string gen_expr = get_json_string(col, "generation_expression_utf8");
    bool is_gcol = !gen_expr.empty();
    if (is_gcol) {
      ss << " GENERATED ALWAYS AS (" << gen_expr << ")";
      if (get_json_bool(col, "is_virtual"))
        ss << " VIRTUAL";
      else
        ss << " STORED";
    }

    /* NULL / NOT NULL */
    if (!get_json_bool(col, "is_nullable")) {
      ss << " NOT NULL";
    } else if (col_type == col_type::TIMESTAMP ||
               col_type == col_type::TIMESTAMP2) {
      ss << " NULL";
    }

    /* NOT SECONDARY */
    std::string col_options_str = get_json_string(col, "options");
    if (!col_options_str.empty()) {
      auto col_opts = parse_options_string(col_options_str);
      auto it = col_opts.find("not_secondary");
      if (it != col_opts.end() && it->second == "1") {
        ss << " NOT SECONDARY";
      }
    }

    /* SRID */
    if (col_type == col_type::GEOMETRY &&
        !get_json_bool(col, "srs_id_null")) {
      ss << " /*!80003 SRID " << get_json_uint64(col, "srs_id") << " */";
    }

    /* DEFAULT */
    bool has_no_default = get_json_bool(col, "has_no_default");
    bool is_auto_inc = get_json_bool(col, "is_auto_increment");
    if (!is_gcol && !has_no_default && !is_auto_inc) {
      std::string default_option = get_json_string(col, "default_option");
      bool def_utf8_null = get_json_bool(col, "default_value_utf8_null");
      std::string def_utf8 = get_json_string(col, "default_value_utf8");

      if (!default_option.empty()) {
        ss << " DEFAULT";
        if (default_option.find("CURRENT_TIMESTAMP") == 0) {
          ss << " " << default_option;
        } else {
          ss << " (" << default_option << ")";
        }
      } else if (def_utf8_null) {
        ss << " DEFAULT NULL";
      } else if (!get_json_bool(col, "default_value_null")) {
        ss << " DEFAULT";
        if (is_numeric_type(col_type)) {
          ss << " " << def_utf8;
        } else {
          ss << " '" << escape_string(def_utf8) << "'";
        }
      }
    }

    /* ON UPDATE */
    std::string update_opt = get_json_string(col, "update_option");
    if (!update_opt.empty()) {
      ss << " ON UPDATE " << update_opt;
    }

    /* AUTO_INCREMENT */
    if (is_auto_inc) {
      ss << " AUTO_INCREMENT";
    }

    /* INVISIBLE */
    if (hidden == col_hidden::HT_HIDDEN_USER) {
      ss << " /*!80023 INVISIBLE */";
    }

    /* COMMENT */
    std::string col_comment = get_json_string(col, "comment");
    if (!col_comment.empty()) {
      ss << " COMMENT '" << escape_string(col_comment) << "'";
    }

    /* ENGINE_ATTRIBUTE */
    std::string col_ea = get_json_string(col, "engine_attribute");
    if (!col_ea.empty()) {
      ss << " /*!80021 ENGINE_ATTRIBUTE '" << escape_string(col_ea) << "' */";
    }

    /* SECONDARY_ENGINE_ATTRIBUTE */
    std::string col_sea = get_json_string(col, "secondary_engine_attribute");
    if (!col_sea.empty()) {
      ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE '" << escape_string(col_sea)
         << "' */";
    }
  }

  /* INDEXES */
  if (dd_obj.HasMember("indexes") && dd_obj["indexes"].IsArray()) {
    const auto &indexes = dd_obj["indexes"];
    for (rapidjson::SizeType i = 0; i < indexes.Size(); i++) {
      const auto &key = indexes[i];
      if (get_json_bool(key, "hidden")) continue;

      int key_type = get_json_int(key, "type");
      bool is_primary = (key_type == idx_type::IT_PRIMARY);

      ss << ",\n  ";
      if (is_primary) {
        ss << "PRIMARY KEY";
      } else if (key_type == idx_type::IT_UNIQUE) {
        ss << "UNIQUE KEY ";
      } else if (key_type == idx_type::IT_FULLTEXT) {
        ss << "FULLTEXT KEY ";
      } else if (key_type == idx_type::IT_SPATIAL) {
        ss << "SPATIAL KEY ";
      } else if (key_type == idx_type::IT_MULTIPLE) {
        ss << "KEY ";
      }

      if (!is_primary) {
        ss << quote_identifier(get_json_string(key, "name"));
      }

      ss << " (";
      bool first_elem = true;
      if (key.HasMember("elements") && key["elements"].IsArray()) {
        const auto &elems = key["elements"];
        for (rapidjson::SizeType j = 0; j < elems.Size(); j++) {
          const auto &elem = elems[j];
          if (get_json_bool(elem, "hidden")) continue;
          if (!first_elem) ss << ",";
          first_elem = false;

          int col_opx = get_json_int(elem, "column_opx");
          if (col_opx >= 0 &&
              col_opx < static_cast<int>(all_columns.size())) {
            const auto &ref_col = all_columns[col_opx];
            if (ref_col.hidden == col_hidden::HT_HIDDEN_SQL) {
              ss << "(" << ref_col.gen_expr_utf8 << ")";
            } else {
              ss << quote_identifier(ref_col.name);

              /* Prefix length: only applicable to string/blob types */
              if (is_string_type(ref_col.type) &&
                  key_type != idx_type::IT_FULLTEXT &&
                  key_type != idx_type::IT_SPATIAL) {
                uint elem_length =
                    static_cast<uint>(get_json_uint64(elem, "length", 0));
                uint char_length =
                    static_cast<uint>(get_json_uint64(
                        columns[col_opx], "char_length", 0));
                if (elem_length > 0 && elem_length < char_length) {
                  CHARSET_INFO *elem_cs =
                      get_charset(static_cast<uint>(ref_col.collation_id),
                                  MYF(0));
                  uint prefix_len = elem_length;
                  if (elem_cs && elem_cs->mbmaxlen > 0) {
                    prefix_len /= elem_cs->mbmaxlen;
                  }
                  if (prefix_len > 0) {
                    ss << "(" << prefix_len << ")";
                  }
                }
              }

              /* DESC */
              if (get_json_int(elem, "order") ==
                  idx_elem_order::ORDER_DESC) {
                ss << " DESC";
              }
            }
          }
        }
      }
      ss << ")";

      /* USING algorithm */
      if (get_json_bool(key, "is_algorithm_explicit")) {
        int algo = get_json_int(key, "algorithm");
        switch (algo) {
          case idx_algo::IA_BTREE:
            ss << " USING BTREE";
            break;
          case idx_algo::IA_HASH:
            ss << " USING HASH";
            break;
          case idx_algo::IA_RTREE:
            if (key_type != idx_type::IT_SPATIAL) ss << " USING RTREE";
            break;
          default:
            break;
        }
      }

      /* KEY_BLOCK_SIZE */
      std::string key_opts_str = get_json_string(key, "options");
      std::map<std::string, std::string> key_opts;
      if (!key_opts_str.empty()) key_opts = parse_options_string(key_opts_str);

      auto kbs_it = key_opts.find("block_size");
      if (kbs_it != key_opts.end() && kbs_it->second != "0") {
        ss << " KEY_BLOCK_SIZE=" << kbs_it->second;
      }

      /* COMMENT */
      std::string key_comment = get_json_string(key, "comment");
      if (!key_comment.empty()) {
        ss << " COMMENT '" << escape_string(key_comment) << "'";
      }

      /* INVISIBLE */
      if (!get_json_bool(key, "is_visible")) {
        ss << " /*!80000 INVISIBLE */";
      }

      /* ENGINE_ATTRIBUTE */
      std::string key_ea = get_json_string(key, "engine_attribute");
      if (!key_ea.empty()) {
        ss << " /*!80021 ENGINE_ATTRIBUTE '" << escape_string(key_ea)
           << "' */";
      }

      /* SECONDARY_ENGINE_ATTRIBUTE */
      std::string key_sea =
          get_json_string(key, "secondary_engine_attribute");
      if (!key_sea.empty()) {
        ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE '"
           << escape_string(key_sea) << "' */";
      }

      /* WITH PARSER */
      auto parser_it = key_opts.find("parser_name");
      if (parser_it != key_opts.end() && !parser_it->second.empty()) {
        ss << " /*!50100 WITH PARSER "
           << quote_identifier(parser_it->second) << " */";
      }
    }
  }

  /* FOREIGN KEYS */
  if (dd_obj.HasMember("foreign_keys") && dd_obj["foreign_keys"].IsArray()) {
    const auto &fks = dd_obj["foreign_keys"];
    for (rapidjson::SizeType i = 0; i < fks.Size(); i++) {
      const auto &fk = fks[i];
      ss << ",\n  CONSTRAINT " << quote_identifier(get_json_string(fk, "name"))
         << " FOREIGN KEY (";

      bool first_fk = true;
      if (fk.HasMember("elements") && fk["elements"].IsArray()) {
        const auto &fk_elems = fk["elements"];
        for (rapidjson::SizeType j = 0; j < fk_elems.Size(); j++) {
          if (!first_fk) ss << ",";
          first_fk = false;
          int col_opx = get_json_int(fk_elems[j], "column_opx");
          if (col_opx >= 0 &&
              col_opx < static_cast<int>(all_columns.size())) {
            ss << quote_identifier(all_columns[col_opx].name);
          }
        }

        ss << ") REFERENCES "
           << quote_identifier(get_json_string(fk, "referenced_table_name"))
           << " (";
        bool first_ref = true;
        for (rapidjson::SizeType j = 0; j < fk_elems.Size(); j++) {
          if (!first_ref) ss << ",";
          first_ref = false;
          ss << quote_identifier(
              get_json_string(fk_elems[j], "referenced_column_name"));
        }
        ss << ")";
      }

      const char *del_rule_str =
          fk_rule_str(get_json_int(fk, "delete_rule"));
      if (del_rule_str) ss << " ON DELETE " << del_rule_str;
      const char *upd_rule_str =
          fk_rule_str(get_json_int(fk, "update_rule"));
      if (upd_rule_str) ss << " ON UPDATE " << upd_rule_str;
    }
  }

  /* CHECK CONSTRAINTS */
  if (dd_obj.HasMember("check_constraints") &&
      dd_obj["check_constraints"].IsArray()) {
    const auto &ccs = dd_obj["check_constraints"];
    for (rapidjson::SizeType i = 0; i < ccs.Size(); i++) {
      const auto &cc = ccs[i];
      ss << ",\n  CONSTRAINT " << quote_identifier(get_json_string(cc, "name"))
         << " CHECK ("
         << unescape_dd_string(get_json_string(cc, "check_clause_utf8"))
         << ")";
      if (get_json_int(cc, "state") == chk_state::CS_NOT_ENFORCED) {
        ss << " /*!80016 NOT ENFORCED */";
      }
    }
  }

  ss << "\n)";

  /* TABLE OPTIONS */
  ss << " ENGINE=" << get_json_string(dd_obj, "engine");

  /* DEFAULT CHARSET / COLLATE */
  int tbl_coll_id = get_json_int(dd_obj, "collation_id");
  CHARSET_INFO *cs =
      get_charset(static_cast<uint>(tbl_coll_id), MYF(0));
  if (cs) {
    ss << " DEFAULT CHARSET=" << cs->csname;
    if (!(cs->state & MY_CS_PRIMARY) ||
        cs == &my_charset_utf8mb4_0900_ai_ci) {
      ss << " COLLATE=" << cs->m_coll_name;
    }
  }

  /* Parse table options */
  std::string tbl_opts_str = get_json_string(dd_obj, "options");
  std::map<std::string, std::string> tbl_opts;
  if (!tbl_opts_str.empty()) tbl_opts = parse_options_string(tbl_opts_str);

  /* PACK_KEYS: stored as HA_OPTION_PACK_KEYS (2) or 0 */
  auto pk_it = tbl_opts.find("pack_keys");
  if (pk_it != tbl_opts.end()) {
    uint64_t pk_val = strtoull(pk_it->second.c_str(), nullptr, 10);
    ss << " PACK_KEYS=" << (pk_val ? "1" : "0");
  }

  /* STATS_PERSISTENT: stored as HA_OPTION_STATS_PERSISTENT (4096) or 0 */
  auto sp_it = tbl_opts.find("stats_persistent");
  if (sp_it != tbl_opts.end()) {
    uint64_t sp_val = strtoull(sp_it->second.c_str(), nullptr, 10);
    ss << " STATS_PERSISTENT=" << (sp_val ? "1" : "0");
  }

  /* STATS_AUTO_RECALC */
  auto sar_it = tbl_opts.find("stats_auto_recalc");
  if (sar_it != tbl_opts.end()) {
    if (sar_it->second == "1")
      ss << " STATS_AUTO_RECALC=1";
    else if (sar_it->second == "2")
      ss << " STATS_AUTO_RECALC=0";
  }

  /* STATS_SAMPLE_PAGES */
  auto ssp_it = tbl_opts.find("stats_sample_pages");
  if (ssp_it != tbl_opts.end() && ssp_it->second != "0") {
    ss << " STATS_SAMPLE_PAGES=" << ssp_it->second;
  }

  /* ROW_FORMAT */
  int rf = get_json_int(dd_obj, "row_format");
  switch (rf) {
    case row_fmt::RF_COMPRESSED:
      ss << " ROW_FORMAT=COMPRESSED";
      break;
    case row_fmt::RF_REDUNDANT:
      ss << " ROW_FORMAT=REDUNDANT";
      break;
    case row_fmt::RF_COMPACT:
      ss << " ROW_FORMAT=COMPACT";
      break;
    default:
      break;
  }

  /* KEY_BLOCK_SIZE */
  auto kbs2_it = tbl_opts.find("key_block_size");
  if (kbs2_it != tbl_opts.end() && kbs2_it->second != "0") {
    ss << " KEY_BLOCK_SIZE=" << kbs2_it->second;
  }

  /* COMPRESSION */
  auto comp_it = tbl_opts.find("compress");
  if (comp_it != tbl_opts.end() && !comp_it->second.empty()) {
    ss << " COMPRESSION='" << comp_it->second << "'";
  }

  /* ENCRYPTION */
  auto enc_it = tbl_opts.find("encrypt_type");
  if (enc_it != tbl_opts.end() && !enc_it->second.empty() &&
      enc_it->second != "N") {
    ss << " ENCRYPTION='" << enc_it->second << "'";
  }

  /* COMMENT */
  std::string tbl_comment = get_json_string(dd_obj, "comment");
  if (!tbl_comment.empty()) {
    ss << " COMMENT='" << escape_string(tbl_comment) << "'";
  }

  /* ENGINE_ATTRIBUTE */
  std::string tbl_ea = get_json_string(dd_obj, "engine_attribute");
  if (!tbl_ea.empty()) {
    ss << " /*!80021 ENGINE_ATTRIBUTE='" << escape_string(tbl_ea) << "' */";
  }

  /* SECONDARY_ENGINE_ATTRIBUTE */
  std::string tbl_sea =
      get_json_string(dd_obj, "secondary_engine_attribute");
  if (!tbl_sea.empty()) {
    ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE='" << escape_string(tbl_sea)
       << "' */";
  }

  /* PARTITIONS */
  int pt = get_json_int(dd_obj, "partition_type");
  if (pt != part_type::PT_NONE) {
    ss << "\n/*!50100 PARTITION BY ";
    std::string part_expr =
        get_json_string(dd_obj, "partition_expression_utf8");

    switch (pt) {
      case part_type::PT_HASH:
        ss << "HASH (" << part_expr << ")";
        break;
      case part_type::PT_LINEAR_HASH:
        ss << "LINEAR HASH (" << part_expr << ")";
        break;
      case part_type::PT_KEY_55:
      case part_type::PT_KEY_51:
        ss << "KEY (" << part_expr << ")";
        break;
      case part_type::PT_LINEAR_KEY_51:
      case part_type::PT_LINEAR_KEY_55:
        ss << "LINEAR KEY (" << part_expr << ")";
        break;
      case part_type::PT_RANGE:
        ss << "RANGE (" << part_expr << ")";
        break;
      case part_type::PT_RANGE_COLUMNS:
        ss << "RANGE COLUMNS(" << part_expr << ")";
        break;
      case part_type::PT_LIST:
        ss << "LIST (" << part_expr << ")";
        break;
      case part_type::PT_LIST_COLUMNS:
        ss << "LIST COLUMNS(" << part_expr << ")";
        break;
      default:
        break;
    }

    int st = get_json_int(dd_obj, "subpartition_type");
    bool has_subparts = st != sub_type::ST_NONE;
    if (has_subparts) {
      std::string subpart_expr =
          get_json_string(dd_obj, "subpartition_expression_utf8");
      switch (st) {
        case sub_type::ST_HASH:
          ss << "\nSUBPARTITION BY HASH (" << subpart_expr << ")";
          break;
        case sub_type::ST_LINEAR_HASH:
          ss << "\nSUBPARTITION BY LINEAR HASH (" << subpart_expr << ")";
          break;
        case sub_type::ST_KEY_55:
        case sub_type::ST_KEY_51:
          ss << "\nSUBPARTITION BY KEY (" << subpart_expr << ")";
          break;
        case sub_type::ST_LINEAR_KEY_51:
        case sub_type::ST_LINEAR_KEY_55:
          ss << "\nSUBPARTITION BY LINEAR KEY (" << subpart_expr << ")";
          break;
        default:
          break;
      }

      if (dd_obj.HasMember("partitions") &&
          dd_obj["partitions"].IsArray()) {
        const auto &parts = dd_obj["partitions"];
        for (rapidjson::SizeType pi = 0; pi < parts.Size(); pi++) {
          const auto &part = parts[pi];
          if (part.HasMember("subpartitions") &&
              part["subpartitions"].IsArray() &&
              part["subpartitions"].Size() > 0) {
            ss << "\nSUBPARTITIONS " << part["subpartitions"].Size();
            break;
          }
        }
      }
    }

    bool need_part_defs =
        (pt == part_type::PT_RANGE || pt == part_type::PT_RANGE_COLUMNS ||
         pt == part_type::PT_LIST || pt == part_type::PT_LIST_COLUMNS);

    if (need_part_defs && dd_obj.HasMember("partitions") &&
        dd_obj["partitions"].IsArray()) {
      ss << "\n(";
      bool first_part = true;
      const auto &parts = dd_obj["partitions"];
      for (rapidjson::SizeType pi = 0; pi < parts.Size(); pi++) {
        const auto &part = parts[pi];
        uint64_t parent_id =
            get_json_uint64(part, "parent_partition_id", UINT64_MAX);
        if (parent_id != UINT64_MAX) continue;

        if (!first_part) ss << ",\n ";
        first_part = false;
        ss << "PARTITION "
           << quote_identifier(get_json_string(part, "name"));

        if (pt == part_type::PT_RANGE || pt == part_type::PT_RANGE_COLUMNS) {
          ss << " VALUES LESS THAN (";
          bool first_val = true;
          if (part.HasMember("values") && part["values"].IsArray()) {
            const auto &vals = part["values"];
            for (rapidjson::SizeType vi = 0; vi < vals.Size(); vi++) {
              if (!first_val) ss << ",";
              first_val = false;
              if (get_json_bool(vals[vi], "max_value"))
                ss << "MAXVALUE";
              else if (get_json_bool(vals[vi], "null_value"))
                ss << "NULL";
              else
                ss << get_json_string(vals[vi], "value_utf8");
            }
          }
          ss << ")";
        } else if (pt == part_type::PT_LIST ||
                   pt == part_type::PT_LIST_COLUMNS) {
          ss << " VALUES IN (";
          bool first_val = true;
          if (part.HasMember("values") && part["values"].IsArray()) {
            const auto &vals = part["values"];
            for (rapidjson::SizeType vi = 0; vi < vals.Size(); vi++) {
              if (!first_val) ss << ",";
              first_val = false;
              if (get_json_bool(vals[vi], "null_value"))
                ss << "NULL";
              else
                ss << get_json_string(vals[vi], "value_utf8");
            }
          }
          ss << ")";
        }

        ss << " ENGINE = " << get_json_string(part, "engine");
      }
      ss << ")";
    } else if (!need_part_defs && dd_obj.HasMember("partitions") &&
               dd_obj["partitions"].IsArray()) {
      auto num_parts = dd_obj["partitions"].Size();
      if (num_parts > 0) ss << "\nPARTITIONS " << num_parts;
    }
    ss << " */";
  }

  ss << ";\n";
  return ss.str();
}

static std::string generate_create_tablespace_from_json(
    const rapidjson::Value &dd_obj) {
  std::ostringstream ss;
  std::string ts_name = get_json_string(dd_obj, "name");

  if (ts_name.find('/') != std::string::npos ||
      ts_name.find("innodb_") == 0 || ts_name == "mysql") {
    return "";
  }

  ss << "CREATE TABLESPACE " << quote_identifier(ts_name);
  if (dd_obj.HasMember("files") && dd_obj["files"].IsArray()) {
    const auto &files = dd_obj["files"];
    for (rapidjson::SizeType i = 0; i < files.Size(); i++) {
      ss << " ADD DATAFILE '" << get_json_string(files[i], "filename")
         << "'";
      break;
    }
  }
  ss << " ENGINE = " << get_json_string(dd_obj, "engine");
  ss << ";\n";
  return ss.str();
}

static void process_sdi_create_sql(const char *sdi_data, uint64_t sdi_data_len,
                                   const char *ibd_path,
                                   std::string &accumulated_sql) {
  rapidjson::Document doc;
  rapidjson::ParseResult ok =
      doc.Parse(sdi_data, static_cast<size_t>(sdi_data_len - 1));
  if (!ok) {
    std::cerr << "JSON parse error: "
              << rapidjson::GetParseError_En(ok.Code()) << " (offset "
              << ok.Offset() << ")" << std::endl;
    return;
  }

  if (!doc.HasMember("dd_object") || !doc["dd_object"].IsObject()) {
    std::cerr << "SDI record missing dd_object" << std::endl;
    return;
  }

  const auto &dd_obj = doc["dd_object"];
  std::string obj_type = get_json_string(doc, "dd_object_type");

  if (obj_type == "Table") {
    std::string sql = generate_create_table_from_json(dd_obj);
    if (!sql.empty()) accumulated_sql += sql;
  } else if (obj_type == "Tablespace") {
    std::string sql = generate_create_tablespace_from_json(dd_obj);
    if (!sql.empty()) accumulated_sql += sql;
  }
}

static void write_sql_output(const std::string &sql,
                              const char *ibd_path) {
  /* Print to stdout */
  std::cout << sql;

  /* Write .sql file alongside .ibd */
  std::string path(ibd_path);
  auto dot_pos = path.rfind('.');
  std::string sql_path;
  if (dot_pos != std::string::npos)
    sql_path = path.substr(0, dot_pos) + ".sql";
  else
    sql_path = path + ".sql";

  std::ofstream sql_file(sql_path);
  if (sql_file.is_open()) {
    sql_file << sql;
    sql_file.close();
    std::cerr << "Wrote " << sql_path << std::endl;
  } else {
    std::cerr << "Error: cannot write " << sql_path << std::endl;
  }
}

}  // namespace create_sql

/** Error logging classes. */
namespace ib {

logger::~logger() = default;

info::~info() {
  std::cerr << "[INFO] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

warn::~warn() {
  std::cerr << "[WARNING] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

error::~error() {
  std::cerr << "[ERROR] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

/*
MSVS complains: Warning C4722: destructor never returns, potential memory leak.
But, the whole point of using ib::fatal temporary object is to cause an abort.
*/
MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4722)

fatal::~fatal() {
  std::cerr << "[FATAL] ibd2sdi: " << m_oss.str() << "." << std::endl;
  ut_error;
}

// Restore the MSVS checks for Warning C4722, silenced for ib::fatal::~fatal().
MY_COMPILER_DIAGNOSTIC_POP()

/* TODO: Improve Object creation & destruction on NDEBUG */
class dbug : public logger {
 public:
  ~dbug() override { DBUG_PRINT("ibd2sdi", ("%s", m_oss.str().c_str())); }
};
}  // namespace ib

/** Check if page is corrupted or not.
@param[in]	buf		page frame
@param[in]	page_size	page size
@retval		true		if page is corrupted
@retval		false		if page is not corrupted */
static bool is_page_corrupted(const byte *buf, const page_size_t &page_size) {
  BlockReporter reporter(false, buf, page_size, opts.strict_check);

  return (reporter.is_corrupted());
}

/** Seek the file pointer to page offset in the file.
@param[in]	file_in		File pointer
@param[in]	page_size	page size
@param[in]	page_num	page number
@retval		true		success
@retval		false		error */
static bool seek_to_page(File file_in, const page_size_t &page_size,
                         page_no_t page_num) {
  my_off_t offset = page_num * page_size.physical();

  DBUG_EXECUTE_IF("ib_seek_error", offset = -1;);
  if (my_seek(file_in, offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    ib::error() << "Error: Unable to seek to necessary offset for"
                   " file with descriptor "
                << file_in
                << " and error msg"
                   " is: "
                << strerror(errno);
    return (false);
  }

  return (true);
}

/** Read physical page size from file into buffer.
@param[in]	page_num	page number
@param[in]	page_size	page size of the tablespace
@param[in]	buf_len		length of buffer
@param[in,out]	buf		read the file in buffer
@param[in,out]	file_in		file pointer created for the tablespace
@return number of bytes read or IB_ERROR on error */
static size_t read_page(page_no_t page_num, const page_size_t &page_size,
                        uint32_t buf_len, byte *buf, File file_in) {
  if (!seek_to_page(file_in, page_size, page_num)) {
    return (IB_ERROR);
  }

  size_t physical_page_size = static_cast<size_t>(page_size.physical());

  ut_ad(physical_page_size >= UNIV_ZIP_SIZE_MIN);
  ut_ad(buf_len >= physical_page_size);

  size_t n_bytes_read = my_read(file_in, buf, physical_page_size, MYF(0));
  ut_ad(n_bytes_read == physical_page_size || n_bytes_read == IB_ERROR);

  return (n_bytes_read);
}

/** Datafile information. */
typedef struct ib_file {
  /** 0 in fil_per_table tablespaces, else the first page number in
  subsequent data file in multi-file tablespace. */
  page_no_t first_page_num;
  /** Total number of pages in a data file. */
  page_no_t tot_num_of_pages;
  /** File handle of the data file. */
  File file_handle;
} ib_file_t;

/** Class to hold information about a single InnoDB tablespace. */
class ib_tablespace {
 public:
  /** Constructor from space_id & page_size.
  @param[in]	space_id	tablespace id
  @param[in]	page_size	tablespace page size */
  ib_tablespace(space_id_t space_id, const page_size_t &page_size)
      : m_space_id(space_id),
        m_page_size(page_size),
        m_file_vec(),
        m_page_num_recs(0),
        m_max_recs_per_page(page_size.logical() / SDI_REC_SIZE),
        m_sdi_root(0),
        m_tot_pages(0) {}

  /** Destructor. Closes open file handles of Datafiles. */
  ~ib_tablespace() {
    std::vector<ib_file>::const_iterator it;
    for (it = m_file_vec.begin(); it != m_file_vec.end(); ++it) {
      File file_handle = it->file_handle;
      if (file_handle != -1) {
        my_close(file_handle, MYF(0));
      }
    }
  }

  /** Copy Constructor.
  @param[in]	copy	another object of ib_tablespace */
  ib_tablespace(const ib_tablespace &copy) = default;

  /** Add Datafile to vector of datafiles. Also
  resize of vector of pages.
  @param[in]	data_file	Datafile */
  inline void add_data_file(const ib_file_t &data_file) {
    m_file_vec.push_back(data_file);

    m_page_num_recs.resize(m_page_num_recs.size() +
                               static_cast<size_t>(data_file.tot_num_of_pages),
                           0);
    m_tot_pages += data_file.tot_num_of_pages;
  }

  /** Add the SDI root page numbers to tablespace.
  @param[in]	root	root page number of SDI */
  inline void add_sdi(page_no_t root) { m_sdi_root = root; }

  /** @return SDI root page number */
  inline page_no_t get_sdi_root() const { return (m_sdi_root); }

  /** Return space id of the tablespace.
  @return	space id of the tablespace */
  inline space_id_t get_space_id() const { return (m_space_id); }

  /** Return the page size of the tablespace.
  @return the page size of the tablespace. */
  inline const page_size_t &get_page_size() const { return (m_page_size); }

  /** Return the number of data files of tablespace.
  @return the number of data files of tablespace */
  inline uint64_t get_file_count() const { return (m_file_vec.size()); }

  /** Return the first page number of nth Datafile.
  @param[in]	df_num	Datafile number
  @return the first page number of nth Datafile */
  inline ib_file_t get_nth_data_file(uint64_t df_num) const {
    ut_ad(df_num < m_file_vec.size());
    return (m_file_vec[static_cast<unsigned>(df_num)]);
  }

  /** Increment the number of records on a page. This counter is
  used to detect page corruption. Every page has limit on maximum
  number of records stored.
  @param[in]	page_num	Page on which record is found
  @return true if the number of records exceed the max limit(i.e
  corruption detected), else return false. */
  bool inc_num_of_recs_on_page(page_no_t page_num) {
    ut_ad(page_num < m_page_num_recs.size());
    ++m_page_num_recs[page_num];

    if (m_page_num_recs[page_num] > get_max_recs_per_page()) {
      ib::error() << "Record Corruption detected. Too many"
                     " records or infinite loop detected. Aborting";
      ib::error() << "The current iteration num is "
                  << m_page_num_recs[page_num]
                  << ". Maximum number of records expected on"
                  << " the page " << page_num << " is "
                  << get_max_recs_per_page();
      return (true);
    }
    return (false);
  }

  /** Get the maximum number of records possible on a page.
  @return the maximum possible number of records on a page */
  inline uint64_t get_max_recs_per_page() const {
    return (m_max_recs_per_page);
  }

  /** Get the number of records on a page.
  @param[in]	page_num	Page number
  @return the number of records on page */
  inline uint64_t get_cur_num_recs_on_page(page_no_t page_num) const {
    ut_ad(page_num < m_page_num_recs.size());
    return (m_page_num_recs[page_num]);
  }

  /** Return the file handle for which the page belongs. This
  is applicable for multi-file tablespaces (like ibdata*)
  @param[in]	page_num		Page number
  @param[in,out]	offset_in_datafile	offset of page
                                          in data file
  @return the File handle if found, else -1. */
  inline File get_file_handle_for_page(page_no_t page_num,
                                       page_no_t *offset_in_datafile) const {
    /* Now find which datafile of the tablespace to use for reading
    the page. */

    File file_in = -1;
    *offset_in_datafile = FIL_NULL;

    for (uint32_t i = 0; i < get_file_count(); i++) {
      ib_file_t file = get_nth_data_file(i);
      if (page_num < file.first_page_num + file.tot_num_of_pages) {
        file_in = file.file_handle;
        *offset_in_datafile = page_num - file.first_page_num;
        break;
      }
    }
    return (file_in);
  }

  /** Check if SDI exists in a tablespace. If SDI exists, retrieve
  SDI root page number.
  @param[in,out]	root	SDI root page number
  @return false on success, true on failure */
  inline bool check_sdi(page_no_t &root);

  /** Return the total number of pages of the tablespaces.
  This includes pages of all datafiles (ibdata*)
  @return total number of pages of tablespace */
  inline page_no_t get_tot_pages() const { return (m_tot_pages); }

 private:
  /** Return the SDI Root page number stored in a page.
  @param[in]	buf		Page read from buffer
  @return SDI root page number */
  inline page_no_t get_sdi_root_page_num(byte *buf);

  /** Space id of tablespace. */
  space_id_t m_space_id;

  /** Page size of tablespace. */
  page_size_t m_page_size;

  /** Vector of datafiles of a tablespace. */
  std::vector<ib_file_t> m_file_vec;

  /** Vector of pages. For each page, the number of records
  found on each page of tablespace is stored in vector. */
  std::vector<uint64_t> m_page_num_recs;

  /** Maximum number of records possible on a page. */
  uint64_t m_max_recs_per_page;

  /** Root page number of SDI. */
  page_no_t m_sdi_root;

  /** Total number of pages of all data files. */
  page_no_t m_tot_pages;
};

/** Read page from file into memory. If page is compressed SDI page,
decompress and store the uncompressed copy in the buffer.
@param[in]	ts		tablespace structure
@param[in]	page_num	page number
@param[in]	buf_len		buf_len
@param[in,out]	buf		the page read will stored in this buffer
@return number of bytes read from file on success, else return IB_ERROR on
failure */
static size_t fetch_page(ib_tablespace *ts, page_no_t page_num,
                         uint32_t buf_len, byte *buf) {
  DBUG_TRACE;
  ib::dbug() << "Read page number: " << page_num;

  const page_size_t &page_size = ts->get_page_size();

  /* Get last data file first page_num & total_pages in the
  datafile to check the bounds. */

  ut_ad(ts->get_file_count() > 0);

  DBUG_EXECUTE_IF("ib_invalid_page", page_num = ts->get_tot_pages(););

  if (page_num >= ts->get_tot_pages()) {
    ib::error() << "Read requested on invalid page number " << page_num
                << ". The maximum valid page number"
                << " in the tablespace is " << ts->get_tot_pages() - 1;
    return IB_ERROR;
  }

  /* Now find which datafile of the tablespace to use for reading
  the page. */

  page_no_t offset_in_datafile;
  File file_in = ts->get_file_handle_for_page(page_num, &offset_in_datafile);

  ut_ad(file_in != -1);
  ut_ad(buf_len >= page_size.physical());

  size_t n_bytes = IB_ERROR;
  memset(buf, 0, page_size.physical());

  n_bytes = read_page(offset_in_datafile, page_size, buf_len, buf, file_in);

  if (n_bytes == IB_ERROR) {
    return IB_ERROR;
  }

  if (!opts.no_checksum) {
    bool corrupt_status = is_page_corrupted(buf, page_size);

    if (corrupt_status) {
      page_id_t page_id(ts->get_space_id(), page_num);

      ib::error() << "Page " << page_id
                  << " is corrupted."
                     " Checksum verification failed";
      return IB_ERROR;
    }
  }

  if (page_size.is_compressed() && fil_page_get_type(buf) == FIL_PAGE_SDI) {
    byte *uncomp_buf = static_cast<byte *>(ut::malloc(2 * page_size.logical()));

    byte *uncomp_page =
        static_cast<byte *>(ut_align(uncomp_buf, page_size.logical()));

    memset(uncomp_page, 0, page_size.logical());
    page_zip_des_t page_zip;

    page_zip_des_init(&page_zip);
    page_zip.data = buf;
    page_zip.ssize = page_size_to_ssize(page_size.physical());

    DBUG_EXECUTE_IF("ib_decompress_fail",
                    mach_write_to_2(buf + PAGE_HEADER + PAGE_N_HEAP, 10000););

    page_id_t page_id(ts->get_space_id(), page_num);

    if (!page_zip_decompress_low(&page_zip, uncomp_page, true)) {
      /* To indicate error */
      n_bytes = IB_ERROR;
      ib::error() << "Decompression failed for"
                     " compressed page "
                  << page_id;
    } else {
      ib::dbug() << "Decompression Success for"
                    " compressed page "
                 << page_id;

      /* Page is decompressed. */
      memset(buf, 0, buf_len);
      ut_ad(buf_len >= page_size.logical());
      memcpy(buf, uncomp_page, page_size.logical());
    }

    ut::free(uncomp_buf);
  }

  return n_bytes;
}

class tablespace_creator {
 public:
  /** Constructor
  @param[in]	num_files	number of ibd files
  @param[in]	ibd_files	array of ibd file names */
  tablespace_creator(uint32_t num_files, char **ibd_files)
      : m_num_files(num_files), m_ibd_files(ibd_files), m_tablespace(nullptr) {}

  /* Destructor */
  ~tablespace_creator() { delete m_tablespace; }

  /** Create tablespace from the ibd files passed.
  @return false on success, true on failure */
  bool create();

  /** Return the tablespace object created from files.
  @return tablespace object */
  inline ib_tablespace *get_tablespace() { return (m_tablespace); }

 private:
  /** Determine page_size by reading MAX_PAGES_TO_SCAN pages (or actual
  number of pages if less) and verifying the checksums.
  @param[in]	file_in		File pointer
  @param[in,out]	page_size	determined page size
  @return true if valid page_size is determined, else false */
  bool determine_page_size(File file_in, page_size_t &page_size);

  /** Determine the minimum corruption ratio and update page_size.
  @param[in]	file_in		file pointer
  @param[in]	num_pages	Total number pages to verify checksum
  @param[in]	in_page_size	the current page_size used for checksum
  @param[in,out]	final_page_size	updated to in_page_size if the current
                                  corruption ratio is less than minimum
                                  corruption ratio
  @param[in,out] min_corr_ratio	updated if the current page_size
                                  corruption ratio is less than
                                  current value.*/
  void determine_min_corruption_ratio(File file_in, page_no_t num_pages,
                                      const page_size_t &in_page_size,
                                      page_size_t &final_page_size,
                                      double *min_corr_ratio);

  /** Verify checksum of the pages and return corruption ratio
  @param[in]	file_in		file pointer
  @param[in]	page_size	page size
  @param[in]	num_pages	Total number pages to verify checksum */
  double get_page_size_corruption_count(File file_in,
                                        const page_size_t &page_size,
                                        page_no_t num_pages);

  /** Find the page_size from the map with the least corruption count.
  @param[in,out]	page_size	page size structure to hold determined
                                  page size */
  void find_best_page_size(page_size_t &page_size);

  /** Get the page size of the tablespace from the tablespace header.
  If tablespace header is corrupted, we will determine the page size by
  reading some other pages and calculating checksums.
  @param[in]	buf		buffer which contains first page of tablespace
  @param[in]	file_in		File handle
  @param[in,out]	page_size	page_size that is determined
  @return true if page_size is determined, else false */
  bool get_page_size(const byte *buf, File file_in, page_size_t &page_size);

  /** The total number of files passed to the tool. */
  uint32_t m_num_files;
  /** The files passed to the tool. */
  char **m_ibd_files;
  /** Tablespace object created from the list of file passed to
  the tool. */
  ib_tablespace *m_tablespace;
};

/** Create tablespace from the ibd files passed.
@return false on success, true on failure */
bool tablespace_creator::create() {
  byte buf[UNIV_ZIP_SIZE_MIN];
  memset(buf, 0, UNIV_ZIP_SIZE_MIN);

  DBUG_TRACE;

  for (uint32_t i = 0; i < m_num_files; i++) {
    const char *filename = m_ibd_files[i];
    MY_STAT stat_info;

    /* stat the file to get size and page count. */
    if (my_stat(filename, &stat_info, MYF(0)) == nullptr) {
      ib::error() << "Unable to get file stats " << filename;
      ib::error() << "File doesn't exist";
      return true;
    }

    uint64_t size = stat_info.st_size;
    DBUG_EXECUTE_IF("ib_file_open_error", filename = "junk_file";);
    File file_in = my_open(filename, O_RDONLY, MYF(0));
    /* If file_in is NULL, terminate as some error encountered. */
    if (file_in == -1) {
      ib::error() << "Unable to open file " << filename;
      return true;
    }

    /* Read minimum page_size. */
    size_t bytes = my_read(file_in, buf, UNIV_ZIP_SIZE_MIN, MYF(0));

    if (bytes != UNIV_ZIP_SIZE_MIN) {
      ib::error() << " Unable to read the page header"
                  << " of " << UNIV_ZIP_SIZE_MIN << " bytes";
      if (bytes != IB_ERROR) {
        ib::error() << " Bytes read was " << bytes;
      }
      my_close(file_in, MYF(0));
      return true;
    }

    const space_id_t space_id =
        mach_read_from_4(buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

    const page_no_t first_page_num = mach_read_from_4(buf + FIL_PAGE_OFFSET);

    ib::dbug() << "The space id of the file " << filename << " is " << space_id;

    if (i == 0) {
      /* First data file of system tablespace
      or single table tablespace */

      page_size_t page_size(univ_page_size);

      bool success = get_page_size(buf, file_in, page_size);

      if (!success) {
        return true;
      }

      ut_ad(first_page_num == 0);

      page_no_t pages = static_cast<page_no_t>(size / page_size.physical());

      DBUG_EXECUTE_IF("ib_partial_page", size++;);

      if (pages * page_size.physical() != size) {
        ib::warn() << "There is a partial page at the"
                   << " end, of size " << size - (pages * page_size.physical())
                   << ". This partial page is ignored";
      }

      ib::dbug() << "Total Number of pages in the file: " << pages;

      ib_file_t ibd_file;
      ibd_file.first_page_num = first_page_num;
      ibd_file.file_handle = file_in;
      ibd_file.tot_num_of_pages = pages;

      m_tablespace = new ib_tablespace(space_id, page_size);
      m_tablespace->add_data_file(ibd_file);

      page_no_t root;
      if (m_tablespace->check_sdi(root)) {
        ib::error() << "SDI doesn't exist for"
                       " this tablespace or the SDI"
                       " root page numbers couldn't"
                       " be determined";
        return true;
      }
      m_tablespace->add_sdi(root);

    } else {
      /* We found next file of system tablespace. */
      ut_ad(m_tablespace != nullptr);

      if (space_id != m_tablespace->get_space_id()) {
        ib::error() << "Multiple tablespaces passed."
                    << " Please specify only one"
                    << " tablespace";
        my_close(file_in, MYF(0));
        return true;
      }

      /* This datafile should belong system tablespace
      only. */
      ut_ad(space_id == 0);

      const page_size_t &page_size = m_tablespace->get_page_size();
      uint64_t phys_page_size = page_size.physical();
      bool all_zero_page = false;

      byte full_page[UNIV_PAGE_SIZE_MAX];
      memset(full_page, 0, UNIV_ZIP_SIZE_MAX);

      read_page(0, page_size, UNIV_PAGE_SIZE_MAX, full_page, file_in);

      if (buf_page_is_zeroes(full_page, page_size)) {
        all_zero_page = true;
      }

      /* Calculate the last page number of the
      last data_file */
      uint64_t tot_data_files = m_tablespace->get_file_count();

      ib_file_t file = m_tablespace->get_nth_data_file(tot_data_files - 1);

      page_no_t last_file_first_page_num = file.first_page_num;

      page_no_t last_file_tot_pages = file.tot_num_of_pages;

      page_no_t last_page_num_of_last_data_file =
          last_file_first_page_num + last_file_tot_pages - 1;

      if (!all_zero_page &&
          (first_page_num != last_page_num_of_last_data_file + 1)) {
        ib::error() << "The first page num " << first_page_num
                    << " of this"
                       " datafile "
                    << filename << " is not equal to last"
                    << " page num " << last_page_num_of_last_data_file
                    << " + 1 of previous data file."
                    << " Skipping this tablespace";
        my_close(file_in, MYF(0));
        return true;
      }

      ib_file_t ibd_file;
      ibd_file.first_page_num = first_page_num;
      ibd_file.file_handle = file_in;
      ibd_file.tot_num_of_pages = static_cast<page_no_t>(size / phys_page_size);

      /* Add the datafile to the file vector
      of the tablespace */
      m_tablespace->add_data_file(ibd_file);
    }
  }

  return false;
}

/** Get the page size of the tablespace from the tablespace header.
If tablespace header is corrupted, we will determine the page size by
reading some other pages and calculating checksums.
@param[in]	buf		buffer which contains first page of tablespace
@param[in]	file_in		File handle
@param[in,out]	page_size	page_size that is determined
@return true if page_size is determined, else false */
bool tablespace_creator::get_page_size(const byte *buf, File file_in,
                                       page_size_t &page_size) {
  const uint32_t flags = fsp_header_get_flags(buf);
  bool is_valid_flags = fsp_flags_is_valid(flags);

  if (is_valid_flags) {
    const ulint ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);

    if (ssize == 0) {
      srv_page_size = UNIV_PAGE_SIZE_ORIG;
    } else {
      srv_page_size = ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
    }
    srv_page_size_shift = page_size_validate(srv_page_size);
  }

  if (!is_valid_flags || srv_page_size_shift == 0) {
    page_size_t min_valid_size(UNIV_ZIP_SIZE_MIN, UNIV_PAGE_SIZE_MIN, true);

    page_size_t max_valid_size(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX, false);

    /* Page corruption detected. Page size cannot be zero.
    We will read multiple pages to determine the page_size */
    ib::error() << "Page 0 corruption detected. Page size is"
                   " either zero or out of bound";
    ib::error() << "Minimum valid page size is " << min_valid_size;
    ib::error() << "Maximum valid page size is " << max_valid_size;
    ib::error() << "Reading multiple pages to determine the"
                   " page_size";

    bool success = determine_page_size(file_in, page_size);

    if (!success) {
      return (false);
    }

    srv_page_size = static_cast<ulong>(page_size.logical());
    srv_page_size_shift = page_size_validate(srv_page_size);

    ut_ad(srv_page_size_shift != 0);

    univ_page_size.copy_from(page_size_t(srv_page_size, srv_page_size, false));

    return (true);
  }

  ut_ad(srv_page_size_shift != 0);

  univ_page_size.copy_from(page_size_t(srv_page_size, srv_page_size, false));

  page_size.copy_from(page_size_t(flags));
  return (true);
}

/** Determine the minimum corruption ratio and update page_size.
@param[in]	file_in		file pointer
@param[in]	num_pages	Total number pages to verify checksum
@param[in]	in_page_size	the current page_size used for checksum
@param[in,out]	final_page_size	updated to in_page_size if the current
                                corruption ratio is less than minimum
                                corruption ratio
@param[in,out] min_corr_ratio	updated if the current page_size
                                corruption ratio is less than
                                current value.*/
void tablespace_creator::determine_min_corruption_ratio(
    File file_in, page_no_t num_pages, const page_size_t &in_page_size,
    page_size_t &final_page_size, double *min_corr_ratio) {
  double corruption_ratio =
      get_page_size_corruption_count(file_in, in_page_size, num_pages);

  if (corruption_ratio < *min_corr_ratio) {
    *min_corr_ratio = corruption_ratio;
    final_page_size.copy_from(in_page_size);
  }
}

/** Determine page_size by reading MAX_PAGES_TO_SCAN pages (or actual
number of pages if less) and verifying the checksums.
@param[in]	file_in		File pointer
@param[in,out]	page_size	determined page size
@return true if valid page_size is determined, else false */
bool tablespace_creator::determine_page_size(File file_in,
                                             page_size_t &page_size) {
  uint64_t size;
  page_size_t final_page_size(0, 0, false);

  my_seek(file_in, 0, MY_SEEK_END, MYF(0));
  size = my_tell(file_in, MYF(0));
  my_seek(file_in, 0, MY_SEEK_SET, MYF(0));

  double min_corruption_ratio = 1.0;

  for (uint32_t logical_page_size = UNIV_PAGE_SIZE_MIN;
       logical_page_size <= UNIV_PAGE_SIZE_MAX; logical_page_size <<= 1) {
    srv_page_size = logical_page_size;

    for (uint32_t phys_page_size = UNIV_ZIP_SIZE_MIN;
         phys_page_size <= logical_page_size; phys_page_size <<= 1) {
      page_no_t num_pages = size / phys_page_size;

      if (num_pages > MAX_PAGES_TO_SCAN) {
        num_pages = MAX_PAGES_TO_SCAN;
      }

      /* When physical == logical page size, we can have both
      compressed and uncompressed page sizes. For example,
      16k compressed and 16k uncompressed are both valid
      page sizes. */
      if (phys_page_size == logical_page_size) {
        const page_size_t uncomp_page_size(phys_page_size, logical_page_size,
                                           false);

        determine_min_corruption_ratio(file_in, num_pages, uncomp_page_size,
                                       final_page_size, &min_corruption_ratio);
      }

      /* 32k and 64k are not valid compressed page size. */
      if (logical_page_size <= UNIV_ZIP_SIZE_MAX) {
        const page_size_t comp_page_size(phys_page_size, logical_page_size,
                                         true);

        determine_min_corruption_ratio(file_in, num_pages, comp_page_size,
                                       final_page_size, &min_corruption_ratio);
      }
    }
  }

  if (min_corruption_ratio == 1.0) {
    ib::error() << "Page size couldn't be determined";
    return false;
  }
  ib::info() << "Page size determined is : " << final_page_size;
  page_size.copy_from(final_page_size);
  return true;
}

/** Verify checksum of the page and return corruption ratio
@param[in]	file_in		file pointer
@param[in]	page_size	page size
@param[in]	num_pages	Total number pages to verify checksum. */
double tablespace_creator::get_page_size_corruption_count(
    File file_in, const page_size_t &page_size, page_no_t num_pages) {
  uint32_t corruption_count = 0;
  /* We will exclude all-zero pages from total number of pages
  when calculating corruption ratio. */
  page_no_t all_zero_page_count = 0;
  byte buf[UNIV_PAGE_SIZE_MAX];
  memset(buf, 0, UNIV_ZIP_SIZE_MAX);

  for (page_no_t page_num = 0; page_num < num_pages; page_num++) {
    read_page(page_num, page_size, UNIV_PAGE_SIZE_MAX, buf, file_in);

    if (buf_page_is_zeroes(buf, page_size)) {
      ++all_zero_page_count;
      continue;
    }

    bool corrupt = is_page_corrupted(buf, page_size);

    if (corrupt) {
      ++corruption_count;
    }
  }

  // If all pages are all-zero, the corruption ratio is undefined.
  // Return NaN immediately instead of relying on the division below
  // to return NaN. Division by zero is undefined behavior, so make it
  // explicit to avoid complaints from UBSAN.
  if (all_zero_page_count == num_pages)
    return std::numeric_limits<double>::quiet_NaN();

  double corruption_ratio =
      corruption_count * 1.0 / (num_pages - all_zero_page_count);

  return (corruption_ratio);
}

/** Check if SDI exists in a tablespace. If SDI exists, retrieve
SDI root page numbers.
@param[in,out]	root	SDI root page number of copy
@return false on success, true on failure and copy
are filled with IB_ERROR_32 on failure. */
inline bool ib_tablespace::check_sdi(page_no_t &root) {
  DBUG_TRACE;
  root = IB_ERROR_32;

  byte buf[UNIV_PAGE_SIZE_MAX];

  /* Read page 0 from file */
  if (fetch_page(this, 0, UNIV_PAGE_SIZE_MAX, buf) == IB_ERROR) {
    return true;
  }

  ulint space_flags = fsp_header_get_flags(buf);

  ib::dbug() << "flags are " << space_flags;

  bool has_sdi;

  /* 1. Check if SDI flag is set or not */
  if (FSP_FLAGS_HAS_SDI(space_flags)) {
    ib::dbug() << "Tablespace has SDI space flag set. Lets"
                  " read SDI root page number offsets to confirm";
    has_sdi = true;
  } else {
    ib::dbug() << "Tablespace do not have SDI"
                  " space flag set. Lets read SDI root page number"
                  " offsets to confirm";
    has_sdi = false;
  }

  page_no_t sdi_root = get_sdi_root_page_num(buf);

  DBUG_EXECUTE_IF("ib_no_sdi", sdi_root = 0;);

  if (sdi_root == 0) {
    ib::error() << "Couldn't find valid root"
                   " page number";
    return true;
  }

  /* If tablespace flags suggest that we don't have SDI but
  reading from SDI offsets uggest that we have SDI index
  pages, we warn and process the SDI pages. */
  if (!has_sdi) {
    ib::warn() << "Tablespace flags suggest SDI INDEX"
                  " didn't exist but found valid SDI root page"
                  " numbers at SDI offsets in Page 0";
  }

  root = sdi_root;
  return false;
}

/** Return the SDI Root page number stored in a page.
@param[in]	buf		Page of tablespace
@return SDI root page number */
inline page_no_t ib_tablespace::get_sdi_root_page_num(byte *buf) {
  ut_ad(buf != nullptr);
  ulint sdi_offset = fsp_header_get_sdi_offset(m_page_size);
  uint32_t version = mach_read_from_4(buf + sdi_offset);

  if (version != SDI_VERSION) {
    ib::warn() << "Unexpected SDI version. Expected: " << SDI_VERSION
               << " Got: " << version;
  }

  return (mach_read_from_4(buf + sdi_offset + 4));
}

/** Class to dump SDI */
class ibd2sdi {
 public:
  /** Constructor
  @param[in]	num_files	number of ibd files
  @param[in]	ibd_files	array of ibd files
  @param[in,out]	out_stream	Stream to dump SDI
  @param[in]	skip_data	if true, dump only SDI id & type */
  ibd2sdi(uint32_t num_files, char **ibd_files, FILE *out_stream,
          bool skip_data)
      : m_num_files(num_files),
        m_ibd_files(ibd_files),
        m_out_stream(out_stream),
        m_skip_data(skip_data),
        m_is_specific_rec(false),
        m_specific_id(UINT64_MAX),
        m_specific_type(UINT64_MAX),
        m_tablespace_creator(nullptr) {}

  /** Destructor. Close all open files. */
  ~ibd2sdi() { delete m_tablespace_creator; }

  /** Process the files passed in constructor. We do this as
  method instead of constructor because we might fail when processing
  ibd files passed.
  @return false on success, true on failure */
  bool process_files();

  /** Dump SDI in a tablespace
  @return false on success, true on failure */
  bool dump();

  /** Dump SDI of a given SDI id & type
  @param[in]	sdi_id		SDI id
  @param[in]	sdi_type	SDI type
  @return false on success, true on failure */
  bool dump_one_sdi(uint64_t sdi_id, uint64_t sdi_type);

  /** Dump all SDI records matching with SDI id
  @param[in]	sdi_id		SDI id
  @return false on success, true on failure */
  bool dump_matching_ids(uint64_t sdi_id);

  /** Dump all SDI records matching with SDI type
  @param[in]	sdi_type	SDI type
  @return false on success, true on failure */
  bool dump_matching_types(uint64_t sdi_type);

  /** Generate CREATE TABLE SQL from SDI records
  @param[in]	ibd_path	path to the .ibd file
  @return false on success, true on failure */
  bool dump_create_sql(const char *ibd_path);

 private:
  /** Collect SDI records and generate SQL
  @param[in]	ts		tablespace structure
  @param[in]	ibd_path	path to the .ibd file
  @return false on success, true on failure */
  bool create_sql_from_copy(ib_tablespace *ts, const char *ibd_path);
  /** Process SDI from a tablespace
  @param[in]	ts	tablespace structure
  @return false on success, true on failure */
  bool process_sdi_from_copy(ib_tablespace *ts);

  /** Iterate over record from a single SDI copy. There is no comparison
  involved with the records in other copy
  @param[in]	ts		tablespace structure
  @param[in]	root_page_num	SDI root page number
  @param[in]	out_stream	file stream to dump SDI
  @return false on success, true on failure */
  bool dump_all_recs_in_leaf_level(ib_tablespace *ts, page_no_t root_page_num,
                                   FILE *out_stream);

  /** Read page from file into buffer passed and return the page level.
  @param[in]	ts		tablespace structure
  @param[in]	buf_len		buffer length
  @param[in,out]	buf		page read will stored in this buffer
  @param[in]	page_num	page number to read
  @return level of the page read, UINT64_MAX on error */
  uint64_t read_page_and_return_level(ib_tablespace *ts, uint32_t buf_len,
                                      byte *buf, page_no_t page_num);

  /** Read the uncompressed blob stored in off-pages to the buffer.
  @param[in]	ts			tablespace structure
  @param[in]	first_blob_page_num	first blob page number
                                          of the chain
  @param[in]	total_off_page_length	total length of blob stored
                                          in record
  @param[in,out]	dest_buf		blob will be copied to this
                                          buffer
  @return 0 if blob is not read, else the total length of blob read from
  off-pages */
  uint64_t copy_uncompressed_blob(ib_tablespace *ts,
                                  page_no_t first_blob_page_num,
                                  uint64_t total_off_page_length,
                                  byte *dest_buf);

  /** Read the compressed blob stored in off-pages to the buffer.
  @param[in]	ts			tablespace structure
  @param[in]	first_blob_page_num	first blob page number of the
                                          chain
  @param[in]	total_off_page_length	total Length of blob stored
                                          in record
  @param[in,out]	dest_buf		blob will be copied to this
                                          buffer
  @return 0 if blob is not read, else the total length of blob read from
  off-pages */
  uint64_t copy_compressed_blob(ib_tablespace *ts,
                                page_no_t first_blob_page_num,
                                uint64_t total_off_page_length, byte *dest_buf);

  /** Reach to B-Tree level zero and read the first (lefmost) page into
  the buffer
  @param[in]	ts		tablespace structure
  @param[in]	buf_len		buffer length
  @param[in,out]	buf		the page read at level zero will stored
                                  in this buffer
  @param[in]	root_page_num	the root page number of the B-Tree
  @retval		SUCCESS		success
  @retval		FAILURE		failure
  @retval		NO_RECORSDS	zero records found on root page */
  err_t reach_to_leftmost_leaf_level(ib_tablespace *ts, uint32_t buf_len,
                                     byte *buf, page_no_t root_page_num);

  /** Extract SDI record fields
  @param[in]	ts		tablespace structure
  @param[in]	rec		pointer to record
  @param[in,out]	sdi_type	sdi type
  @param[in,out]	sdi_id		sdi id
  @param[in,out]	sdi_data	sdi blob
  @param[in,out]	sdi_data_len	length of sdi blob
  @return DB_SUCCESS on success, else error code */
  dberr_t parse_fields_in_rec(ib_tablespace *ts, byte *rec, uint64_t *sdi_type,
                              uint64_t *sdi_id, byte **sdi_data,
                              uint64_t *sdi_data_len);

  /** Return the record type
  @param[in]	rec	sdi record in a page
  @return	the type of record */
  byte get_rec_type(byte *rec);

  /** Return the location of the next record
  @param[in]	ts		tablespace structure
  @param[in]	current_rec	current sdi record in a page
  @param[in]	buf_len		buffer length
  @param[in]	buf		page containing the current rec
  @param[out]	corrupt		true if corruption detected, else false
  @return location of the next record, else NULL if there are no
  user records */
  byte *get_next_rec(ib_tablespace *ts, byte *current_rec, uint32_t buf_len,
                     byte *buf, bool *corrupt);

  /** Write the extracted SDI record fields to outfile
  if passed, else to stdout
  @param[in]	sdi_type	sdi type
  @param[in]	sdi_id		sdi id
  @param[in]	sdi_data	sdi data
  @param[in]	sdi_data_len	sdi data len
  @param[in]	out_stream	file stream to dump SDI */
  void dump_sdi_rec(uint64_t sdi_type, uint64_t sdi_id, byte *sdi_data,
                    uint64_t sdi_data_len, FILE *out_stream);

  /** Return pointer to the first user record in a page
  @param[in]	ts	tablespace object
  @param[in]	buf_len	buffer length
  @param[in]	buf	Memory containing entire page
  @return pointer to first user record in page */
  byte *get_first_user_rec(ib_tablespace *ts, uint32_t buf_len, byte *buf);

  /** Check the SDI record with user passed SDI record id & type
  and dump only if id & type matches
  @param[in]	sdi_type	sdi type
  @param[in]	sdi_id		sdi id
  @param[in]	sdi_data	sdi data
  @param[in]	sdi_data_len	sdi data len
  @param[in]	out_stream	file stream to dump SDI
  @return true if the record matched with the user passed SDI record */
  bool check_and_dump_record(uint64_t sdi_type, uint64_t sdi_id, byte *sdi_data,
                             uint64_t sdi_data_len, FILE *out_stream);

  /** Number of ibd files. */
  uint32_t m_num_files;
  /** Array of ibd files passed by user. */
  char **m_ibd_files;
  /** Output stream to dump the parsed SDI. */
  FILE *m_out_stream;
  /** True if we just want to dump SDI keys to output
  stream without data. */
  bool m_skip_data;
  /** True if we are looking of specific SDI record. */
  bool m_is_specific_rec;
  /** If we are looking specific SDI record, match this SDI id. */
  uint64_t m_specific_id;
  /** If we are looking specific SDI record, match this SDI type. */
  uint64_t m_specific_type;
  /** tablespace creator class. */
  tablespace_creator *m_tablespace_creator;
};

/** Process SDI from a tablespace
@param[in]	ts	tablespace structure
@return false on success, true on failure */
bool ibd2sdi::process_sdi_from_copy(ib_tablespace *ts) {
  return (dump_all_recs_in_leaf_level(ts, ts->get_sdi_root(), m_out_stream));
}

/** Iterate over record from a single SDI copy. There is no comparison
involved with the records in other copy
@param[in]	ts		tablespace structure
@param[in]	root_page_num	SDI root page number
@param[in]	out_stream	file stream to dump SDI
@return false on success, true on failure */
bool ibd2sdi::dump_all_recs_in_leaf_level(ib_tablespace *ts,
                                          page_no_t root_page_num,
                                          FILE *out_stream) {
  DBUG_TRACE;

  byte buf_unalign[2 * UNIV_PAGE_SIZE_MAX];

  page_size_t page_size(ts->get_page_size());

  byte *buf = static_cast<byte *>(ut_align(buf_unalign, page_size.logical()));

  memset(buf, 0, page_size.logical());

  switch (reach_to_leftmost_leaf_level(ts, page_size.logical(), buf,
                                       root_page_num)) {
    case SUCCESS:
      break;
    case FALIURE:
      return true;
    case NO_RECORDS:
      ib::info() << "SDI is empty";
      return false;
    default:
      ut_ad(0);
  }

  bool explicit_sdi_rec_found = false;
  byte *current_rec = get_first_user_rec(ts, page_size.logical(), buf);
  uint64_t sdi_id;
  uint64_t sdi_type;
  byte *sdi_data = nullptr;
  uint64_t sdi_data_len = 0;
  bool corrupt = false;

  /* Check and Dump records */
  fprintf(out_stream, "%s", "[\"ibd2sdi\"\n");
  while (current_rec != nullptr &&
         get_rec_type(current_rec) != REC_STATUS_SUPREMUM &&
         !explicit_sdi_rec_found && !corrupt) {
    dberr_t err = parse_fields_in_rec(ts, current_rec, &sdi_type, &sdi_id,
                                      &sdi_data, &sdi_data_len);

    if (err == DB_SUCCESS) {
      explicit_sdi_rec_found = check_and_dump_record(sdi_type, sdi_id, sdi_data,
                                                     sdi_data_len, out_stream);

      free(sdi_data);
      sdi_data = nullptr;
    }

    if (explicit_sdi_rec_found) {
      break;
    }

    current_rec =
        get_next_rec(ts, current_rec, page_size.logical(), buf, &corrupt);
  }
  fprintf(out_stream, "%s", "]\n");

  return corrupt;
}

/** Read page from file into buffer passed and return the page level.
@param[in]	ts		tablespace structure
@param[in]	buf_len		buffer length
@param[in,out]	buf		page read will stored in this buffer
@param[in]	page_num	page number to read
@return level of the page read, UINT64_MAX on error */
uint64_t ibd2sdi::read_page_and_return_level(ib_tablespace *ts,
                                             uint32_t buf_len, byte *buf,
                                             page_no_t page_num) {
  /* 1. Read page */
  DBUG_TRACE;
  if (fetch_page(ts, page_num, buf_len, buf) == IB_ERROR) {
    ib::error() << "Couldn't read page " << page_num;
    return UINT64_MAX;
  }
  ib::dbug() << "Read page number: " << page_num;

  ulint page_type = fil_page_get_type(buf);

  if (page_type != FIL_PAGE_SDI) {
    ib::error() << "Unexpected page type: " << page_type
                << ". Expected page type:" << FIL_PAGE_SDI;
    return UINT64_MAX;
  }

  /* 2. Get page level */
  ulint page_level = mach_read_from_2(buf + FIL_PAGE_DATA + PAGE_LEVEL);

  ib::dbug() << "page level is " << page_level;
  return page_level;
}

/** Read the uncompressed blob stored in off-pages to the buffer.
@param[in]	ts			tablespace structure
@param[in]	first_blob_page_num	first blob page number of the chain
@param[in]	total_off_page_length	total length of blob stored in record
@param[in,out]	dest_buf		blob will be copied to this buffer
@return 0 if blob is not read, else the total length of blob read from
off-pages */
uint64_t ibd2sdi::copy_uncompressed_blob(ib_tablespace *ts,
                                         page_no_t first_blob_page_num,
                                         uint64_t total_off_page_length,
                                         byte *dest_buf) {
  DBUG_TRACE;

  byte page_buf[UNIV_PAGE_SIZE_MAX];
  uint64_t calc_length = 0;
  uint64_t part_len;
  page_no_t next_page_num = first_blob_page_num;
  bool error = false;

  do {
    ib::dbug() << "Reading blob from page no " << next_page_num;

    if (fetch_page(ts, next_page_num, UNIV_PAGE_SIZE_MAX, page_buf) ==
        IB_ERROR) {
      ib::error() << "Reading blob page " << next_page_num << " failed";
      error = true;
      break;
    }

    if (fil_page_get_type(page_buf) != FIL_PAGE_SDI_BLOB) {
      ib::error() << "Unexpected BLOB page type " << fil_page_get_type(page_buf)
                  << " found."
                     " Expected BLOB page type is "
                  << FIL_PAGE_SDI_BLOB;
      error = true;
      break;
    }

    part_len =
        mach_read_from_4(page_buf + FIL_PAGE_DATA + lob::LOB_HDR_PART_LEN);

    memcpy(dest_buf + calc_length, page_buf + FIL_PAGE_DATA + lob::LOB_HDR_SIZE,
           static_cast<size_t>(part_len));

    calc_length += part_len;

    next_page_num =
        mach_read_from_4(page_buf + FIL_PAGE_DATA + lob::LOB_HDR_NEXT_PAGE_NO);

    if (next_page_num <= SDI_BLOB_ALLOWED) {
      ib::error() << "Unexpected next BLOB page number. "
                     " The first blob page number cannot start "
                     " before page number "
                  << SDI_BLOB_ALLOWED;
      error = true;
      break;
    }

  } while (next_page_num != FIL_NULL);

  if (!error) {
    ut_ad(calc_length == total_off_page_length);
  }
  return calc_length;
}

/** Read the compressed blob stored in off-pages to the buffer.
@param[in]	ts			tablespace structure
@param[in]	first_blob_page_num	first blob page number of the chain
@param[in]	total_off_page_length	total Length of blob stored in record
@param[in,out]	dest_buf		blob will be copied to this buffer
@return 0 if blob is not read, else the total length of blob read from
off-pages */
uint64_t ibd2sdi::copy_compressed_blob(ib_tablespace *ts,
                                       page_no_t first_blob_page_num,
                                       uint64_t total_off_page_length,
                                       byte *dest_buf) {
  DBUG_TRACE;

  byte page_buf[UNIV_PAGE_SIZE_MAX];
  page_no_t page_num = first_blob_page_num;
  z_stream d_stream;
  int err;
  bool error = false;
  page_size_t page_size = ts->get_page_size();

  d_stream.next_out = dest_buf;
  d_stream.avail_out = static_cast<uInt>(total_off_page_length);
  d_stream.next_in = Z_NULL;
  d_stream.avail_in = 0;

  /* Zlib inflate needs 32KB for the default window size, plus
  a few KB for small objects */
  mem_heap_t *heap = mem_heap_create(40000, UT_LOCATION_HERE);
  page_zip_set_alloc(&d_stream, heap);

  ut_ad(page_size.is_compressed());

  err = inflateInit(&d_stream);
  ut_a(err == Z_OK);

  for (;;) {
    if (fetch_page(ts, page_num, UNIV_PAGE_SIZE_MAX, page_buf) == IB_ERROR) {
      ib::error() << "Reading compressed blob page " << page_num << " failed";
      break;
    }

    if (fil_page_get_type(page_buf) != FIL_PAGE_SDI_ZBLOB) {
      ib::error() << "Unexpected BLOB page type " << fil_page_get_type(page_buf)
                  << " found."
                     " Expected BLOB page type is "
                  << FIL_PAGE_SDI_ZBLOB;
      error = true;
      break;
    }

    page_no_t next_page_num = mach_read_from_4(page_buf + FIL_PAGE_NEXT);
    space_id_t space_id =
        mach_read_from_4(page_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

    d_stream.next_in = page_buf + FIL_PAGE_DATA;
    d_stream.avail_in = static_cast<uInt>(page_size.physical() - FIL_PAGE_DATA);
    err = inflate(&d_stream, Z_NO_FLUSH);
    switch (err) {
      case Z_OK:
        if (!d_stream.avail_out) {
          goto func_exit;
        }
        break;
      case Z_STREAM_END:
        if (next_page_num == FIL_NULL) {
          goto func_exit;
        }
        [[fallthrough]];
      default:
      inflate_error : {
        page_id_t page_id(space_id, page_num);
        ib::error() << "Inflate() of compressed BLOB page " << page_id
                    << " returned " << err << " (" << d_stream.msg << ")";
      }
      case Z_BUF_ERROR:
        goto func_exit;
    }

    if (next_page_num == FIL_NULL || next_page_num <= SDI_BLOB_ALLOWED) {
      if (!d_stream.avail_in) {
        page_id_t page_id(space_id, page_num);
        ib::error() << "Unexpected end of compressed"
                    << " BLOB page " << page_id;
      } else {
        err = inflate(&d_stream, Z_FINISH);
        switch (err) {
          case Z_STREAM_END:
          case Z_BUF_ERROR:
            break;
          default:
            goto inflate_error;
        }
      }
    }

    page_num = next_page_num;
  }

func_exit:
  inflateEnd(&d_stream);
  mem_heap_free(heap);
  if (!error) {
    ut_ad(d_stream.total_out == total_off_page_length);
  }
  return d_stream.total_out;
}

/** Reach to B-Tree level zero and read the first (leftmost) page into
the buffer
@param[in]	ts		tablespace structure
@param[in]	buf_len		buffer length
@param[in,out]	buf		the page read at level zero will stored in
                                this buffer
@param[in]	root_page_num	the root page number of the B-Tree
@retval		SUCCESS		success
@retval		FAILURE		failure
@retval		NO_RECORSDS	zero records found on root page */
err_t ibd2sdi::reach_to_leftmost_leaf_level(ib_tablespace *ts, uint32_t buf_len,
                                            byte *buf,
                                            page_no_t root_page_num) {
  DBUG_TRACE;

  /* 1. Read root page */
  uint64_t page_level =
      read_page_and_return_level(ts, buf_len, buf, root_page_num);

  ib::dbug() << "Root page level is " << page_level;

  if (page_level == UINT64_MAX) {
    ib::error() << "Couldn't reach upto level zero";
    return FALIURE;
  }

  /* 2. Get Number of records in root page */

  ulint num_of_recs = mach_read_from_2(buf + FIL_PAGE_DATA + PAGE_N_RECS);

  if (num_of_recs == 0) {
    ib::dbug() << "Number of records is zero. Nothing to process";
    return NO_RECORDS;
  }

  if (page_level == 0) {
    return SUCCESS;
  }

  page_no_t cur_page_num = root_page_num;
  byte rec_type_byte;
  byte rec_type;
  /* 3. Reach to Level zero */
  while (page_level != 0 && page_level != UINT64_MAX) {
    /* Find infimum record, from infimum, we can find first
    record */

    rec_type_byte = *(buf + PAGE_NEW_INFIMUM - REC_OFF_TYPE);

    /* Retrieve the 3bits to determine the rec type */
    rec_type = rec_type_byte & 0x7;

    if (rec_type != REC_STATUS_INFIMUM) {
      ib::error() << "INFIMUM not found on index page " << cur_page_num;
      break;
    }
    ib::dbug() << "INFIMUM found";

    ulint next_rec_off_t =
        mach_read_from_2(buf + PAGE_NEW_INFIMUM - REC_OFF_NEXT);

    ib::dbug() << "Next record offset is " << next_rec_off_t;

    page_no_t child_page_num =
        mach_read_from_4(buf + PAGE_NEW_INFIMUM + next_rec_off_t +
                         REC_DATA_TYPE_LEN + REC_DATA_ID_LEN);

    ib::dbug() << "Next leftmost child page number is " << child_page_num;

    if (child_page_num < SDI_BLOB_ALLOWED) {
      ib::error() << "Invalid child page number: " << child_page_num
                  << " found";
      return FALIURE;
    }

    uint64_t curr_page_level = page_level;

    page_level = read_page_and_return_level(ts, buf_len, buf, child_page_num);

    if (page_level != curr_page_level - 1) {
      break;
    }
  }

  if (page_level != 0) {
    ib::error() << "Leftmost leaf level page not found"
                << " or invalid";
    return FALIURE;
  }
  ib::dbug() << "Reached leaf level";
  return SUCCESS;
}

/** Extract SDI record fields
@param[in]	ts		tablespace structure
@param[in]	rec		pointer to record
@param[in,out]	sdi_type	sdi type
@param[in,out]	sdi_id		sdi id
@param[in,out]	sdi_data	sdi blob
@param[in,out]	sdi_data_len	length of sdi blob
@return DB_SUCCESS on success, else error code */
dberr_t ibd2sdi::parse_fields_in_rec(ib_tablespace *ts, byte *rec,
                                     uint64_t *sdi_type, uint64_t *sdi_id,
                                     byte **sdi_data, uint64_t *sdi_data_len) {
  DBUG_TRACE;

  if (page_rec_is_infimum(rec) || page_rec_is_supremum(rec)) {
    return DB_CORRUPTION;
  }

  const page_size_t &page_size = ts->get_page_size();

  *sdi_type = mach_read_from_4(rec + REC_OFF_DATA_TYPE);
  *sdi_id = mach_read_from_8(rec + REC_OFF_DATA_ID);
  uint32_t sdi_uncomp_len = mach_read_from_4(rec + REC_OFF_DATA_UNCOMP_LEN);
  uint32_t sdi_comp_len = mach_read_from_4(rec + REC_OFF_DATA_COMP_LEN);

  if (m_skip_data) {
    return DB_SUCCESS;
  }

  byte rec_data_len_partial = *(rec - REC_MIN_HEADER_SIZE - 1);

  uint64_t rec_data_length;
  bool is_rec_data_external = false;
  uint32_t rec_data_in_page_len = 0;

  if (rec_data_len_partial & 0x80) {
    /* Rec length is store in two bytes. Read next
    byte and calculate the total length. */

    rec_data_in_page_len = (rec_data_len_partial & 0x3f) << 8;
    if (rec_data_len_partial & 0x40) {
      is_rec_data_external = true;
      /* Rec is stored externally with 768 byte prefix
      inline */
      rec_data_length =
          mach_read_from_8(rec + REC_OFF_DATA_VARCHAR + rec_data_in_page_len +
                           lob::BTR_EXTERN_LEN);

      rec_data_length += rec_data_in_page_len;
    } else {
      rec_data_length = *(rec - REC_MIN_HEADER_SIZE - 2);
      rec_data_length += rec_data_in_page_len;
    }
  } else {
    /* Rec length is <= 127. Read the length from
    one byte only. */
    rec_data_length = rec_data_len_partial;
  }

  byte *str = static_cast<byte *>(
      calloc(static_cast<size_t>(rec_data_length + 1), sizeof(char)));

  byte *rec_data_origin = rec + REC_OFF_DATA_VARCHAR;

  if (is_rec_data_external) {
    /* TODO: This 768 check should be removed when this tool
    supports only REC_FORMAT_DYNAMIC */
    ut_ad(rec_data_in_page_len == 0 ||
          rec_data_in_page_len == REC_ANTELOPE_MAX_INDEX_COL_LEN);

    if (rec_data_in_page_len != 0) {
      memcpy(str, rec_data_origin, rec_data_in_page_len);
    }

    /* Copy from off-page blob-pages */
    page_no_t first_blob_page_num =
        mach_read_from_4(rec + REC_OFF_DATA_VARCHAR + rec_data_in_page_len +
                         lob::BTR_EXTERN_PAGE_NO);

    uint64_t blob_len_retrieved;
    if (page_size.is_compressed()) {
      blob_len_retrieved = copy_compressed_blob(
          ts, first_blob_page_num, rec_data_length - rec_data_in_page_len,
          str + rec_data_in_page_len);
    } else {
      blob_len_retrieved = copy_uncompressed_blob(
          ts, first_blob_page_num, rec_data_length - rec_data_in_page_len,
          str + rec_data_in_page_len);
    }
    *sdi_data_len = rec_data_in_page_len + blob_len_retrieved;
  } else {
    memcpy(str, rec_data_origin, static_cast<size_t>(rec_data_length));
    *sdi_data_len = rec_data_length;
  }

  *sdi_data_len = rec_data_length;
  *sdi_data = str;

  ut_ad(rec_data_length == sdi_comp_len);

  if (rec_data_length != sdi_comp_len) {
    /* Record Corruption */
    ib::error() << "Record corruption";
    free(str);
    return (DB_CORRUPTION);
  }

  byte *uncompressed_sdi = static_cast<byte *>(calloc(sdi_uncomp_len + 1, 1));
  Sdi_Decompressor decompressor(uncompressed_sdi, sdi_uncomp_len + 1, str,
                                sdi_comp_len);
  decompressor.decompress();

  *sdi_data_len = sdi_uncomp_len + 1;
  *sdi_data = uncompressed_sdi;
  free(str);

  return DB_SUCCESS;
}

/** Return the record type
@param[in]	rec	sdi record in a page
@return	the type of record */
byte ibd2sdi::get_rec_type(byte *rec) {
  byte rec_type_byte = *(rec - REC_OFF_TYPE);

  /* Retrieve the 3bits to determine the rec type */
  return (rec_type_byte & 0x7);
}

/** Return the location of the next record
@param[in]	ts		tablespace structure
@param[in]	current_rec_arg	current sdi record in a page
@param[in]	buf_len		buffer length
@param[in]	buf		page containing the current rec
@param[out]	corrupt		true if corruption detected, else false
@return location of the next record, else NULL if there are no
user records */
byte *ibd2sdi::get_next_rec(ib_tablespace *ts, byte *current_rec_arg,
                            uint32_t buf_len, byte *buf, bool *corrupt) {
  DBUG_TRACE;

  page_no_t page_num = mach_read_from_4(buf + FIL_PAGE_OFFSET);
  bool is_comp = page_is_comp(buf);
  ulint next_rec_offset = rec_get_next_offs(current_rec_arg, is_comp);

  if (next_rec_offset == 0) {
    ib::error() << "Record Corruption detected. Aborting";
    *corrupt = true;
    return nullptr;
  }

  byte *next_rec = buf + next_rec_offset;

  /* Next rec should be within page */
  ut_ad(static_cast<uint32_t>(next_rec - buf) <= buf_len);

  /* If rec is delete marked, skip and fetch next_rec */
  if (rec_get_deleted_flag(next_rec, is_comp) != 0) {
    byte *current_rec = next_rec;
    return get_next_rec(ts, current_rec, buf_len, buf, corrupt);
  }

  if (get_rec_type(next_rec) == REC_STATUS_SUPREMUM) {
    /* Reached last record on current page.
    Fetch record from next_page */

    if (memcmp(next_rec, "supremum", strlen("supremum")) != 0) {
      ib::warn() << "supremum record payload on page " << page_num
                 << " is corrupted";
    }

    ulint supremum_next_rec_off = mach_read_from_2(next_rec - REC_OFF_NEXT);

    if (supremum_next_rec_off != 0) {
      ib::warn() << "Unexpected next-rec offset " << supremum_next_rec_off
                 << " of supremum record on page " << page_num;
    }

    page_no_t next_page_num = mach_read_from_4(buf + FIL_PAGE_NEXT);

    if (next_page_num == FIL_NULL) {
      *corrupt = false;
      return nullptr;
    }

    if (fetch_page(ts, next_page_num, buf_len, buf) == IB_ERROR) {
      ib::error() << "Couldn't read next page " << next_page_num;
      *corrupt = true;
      return nullptr;
    }
    ib::dbug() << "Read page number: " << next_page_num;

    ulint page_type = fil_page_get_type(buf);

    if (page_type != FIL_PAGE_SDI) {
      ib::error() << "Unexpected page type: " << page_type
                  << ". Expected page type: " << FIL_PAGE_SDI;
      *corrupt = true;
      return nullptr;
    }

    if (ts->inc_num_of_recs_on_page(next_page_num)) {
      *corrupt = true;
      return nullptr;
    }
    next_rec = get_first_user_rec(ts, buf_len, buf);
  } else {
    if (ts->inc_num_of_recs_on_page(page_num)) {
      *corrupt = true;
      return nullptr;
    }
  }

  *corrupt = false;

  return next_rec;
}

/** Write the extracted SDI record fields to outfile
if passed, else to stdout.
@param[in]	sdi_type	sdi type
@param[in]	sdi_id		sdi id
@param[in]	sdi_data	sdi data
@param[in]	sdi_data_len	sdi data len
@param[in]	out_stream	file stream to dump SDI */
void ibd2sdi::dump_sdi_rec(uint64_t sdi_type, uint64_t sdi_id, byte *sdi_data,
                           uint64_t sdi_data_len, FILE *out_stream) {
  fprintf(out_stream, ",\n");
  fprintf(out_stream, "{\n");
  fprintf(out_stream, "\t\"type\": " UINT64PF ",\n", sdi_type);
  fprintf(out_stream, "\t\"id\": " UINT64PF, sdi_id);

  if (!m_skip_data) {
    ut_ad(sdi_data != nullptr);
    fprintf(out_stream, ",\n");
    fprintf(out_stream, "\t\"object\":\n");
    fprintf(out_stream, "\t\t");
    char *sdi = reinterpret_cast<char *>(sdi_data);

    if (opts.pretty) {
      rapidjson::Document d;

      rapidjson::ParseResult ok = d.Parse(sdi);
      if (!ok) {
        std::cerr << "JSON parse error: "
                  << rapidjson::GetParseError_En(ok.Code()) << " (offset "
                  << ok.Offset() << ")"
                  << " sdi: " << sdi << std::endl;
        exit(1);
      }

      rapidjson::StringBuffer _b;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(_b);
      d.Accept(writer);
      fprintf(out_stream, "%s", _b.GetString());
    } else {
      if (fwrite(sdi_data, 1, static_cast<size_t>(sdi_data_len - 1),
                 out_stream) != static_cast<size_t>(sdi_data_len - 1)) {
        std::cerr << "File write error: " << ferror(out_stream) << std::endl;
        exit(1);
      }
    }
  }

  fprintf(out_stream, "\n}\n");
  fflush(out_stream);
}

/** Return pointer to the first user record in a page
@param[in]	ts	tablespace object
@param[in]	buf_len	buffer length
@param[in]	buf	Memory containing entire page
@return pointer to first user record in page or NULL
if corruption detected. */
byte *ibd2sdi::get_first_user_rec(ib_tablespace *ts, uint32_t buf_len,
                                  byte *buf) {
  DBUG_TRACE;
  ulint next_rec_off_t =
      mach_read_from_2(buf + PAGE_NEW_INFIMUM - REC_OFF_NEXT);

  /* First user record shouldn't be supremum */
  ut_ad(PAGE_NEW_INFIMUM + next_rec_off_t != PAGE_NEW_SUPREMUM);

  if (next_rec_off_t > buf_len) {
    ut_ad(0);
    return (nullptr);
  }

  if (memcmp(buf + PAGE_NEW_INFIMUM, "infimum", strlen("infimum")) != 0) {
    ib::warn() << "Infimum payload on page "
               << mach_read_from_4(buf + FIL_PAGE_OFFSET) << " is corrupted";
  }

  ib::dbug() << "Next record offset is " << next_rec_off_t;

  byte *current_rec = buf + PAGE_NEW_INFIMUM + next_rec_off_t;

  /* current rec should be within page */
  ut_ad(static_cast<uint32_t>(current_rec - buf) <= buf_len);

  bool is_comp = page_is_comp(buf);
  /* record is delete marked, get next record */
  if (rec_get_deleted_flag(current_rec, is_comp) != 0) {
    page_size_t page_size(ts->get_page_size());
    bool corrupt;
    current_rec =
        get_next_rec(ts, current_rec, page_size.logical(), buf, &corrupt);
    if (corrupt) {
      return nullptr;
    }
  }

  return current_rec;
}

/** Check the SDI record with user passed SDI record id & type
and dump only if id & type matches
@param[in]	sdi_type	sdi type
@param[in]	sdi_id		sdi id
@param[in]	sdi_data	sdi data
@param[in]	sdi_data_len	sdi data len
@param[in]	out_stream	file stream to dump SDI
@return true if the record matched with the user passed SDI record */
bool ibd2sdi::check_and_dump_record(uint64_t sdi_type, uint64_t sdi_id,
                                    byte *sdi_data, uint64_t sdi_data_len,
                                    FILE *out_stream) {
  bool explicit_sdi_rec_found = false;

  if (m_is_specific_rec) {
    if (m_specific_type == sdi_type && m_specific_id == sdi_id) {
      explicit_sdi_rec_found = true;
      dump_sdi_rec(sdi_type, sdi_id, sdi_data, sdi_data_len, out_stream);
    } else if (sdi_type > m_specific_type && sdi_id > m_specific_id) {
      /* This is set to make search for specific SDI
      record end faster. */
      explicit_sdi_rec_found = true;
    }
  } else {
    if (m_specific_type == sdi_type || m_specific_id == sdi_id ||
        (m_specific_type == UINT64_MAX && m_specific_id == UINT64_MAX)) {
      dump_sdi_rec(sdi_type, sdi_id, sdi_data, sdi_data_len, out_stream);
    }
  }
  return (explicit_sdi_rec_found);
}

/** Process the files passed in constructor. We do this as
method instead of constructor because we might fail when processing
ibd files passed.
@return false on success, true on failure */
bool ibd2sdi::process_files() {
  m_tablespace_creator = new tablespace_creator(m_num_files, m_ibd_files);
  return (m_tablespace_creator->create());
}

/** Dump SDI in a tablespace
@return false on success, true on failure */
bool ibd2sdi::dump() {
  bool ret = true;
  ut_a(m_tablespace_creator != nullptr);
  ib_tablespace *ts = m_tablespace_creator->get_tablespace();
  ret = process_sdi_from_copy(ts);
  return (ret);
}

/** Dump SDI of a given SDI id & type
@param[in]	sdi_id		SDI id
@param[in]	sdi_type	SDI type
@return false on success, true on failure */
bool ibd2sdi::dump_one_sdi(uint64_t sdi_id, uint64_t sdi_type) {
  m_specific_id = sdi_id;
  m_specific_type = sdi_type;
  m_is_specific_rec = true;

  bool ret = dump();

  m_specific_id = UINT64_MAX;
  m_specific_id = UINT64_MAX;
  m_is_specific_rec = false;

  return (ret);
}

/** Dump all SDI records matching with SDI id
@param[in]	sdi_id		SDI id
@return false on success, true on failure */
bool ibd2sdi::dump_matching_ids(uint64_t sdi_id) {
  m_specific_id = sdi_id;
  bool ret = dump();
  m_specific_id = UINT64_MAX;

  return (ret);
}

/** Dump all SDI records matching with SDI type
@param[in]	sdi_type	SDI type
@return false on success, true on failure */
bool ibd2sdi::dump_matching_types(uint64_t sdi_type) {
  m_specific_type = sdi_type;
  bool ret = dump();
  m_specific_type = UINT64_MAX;

  return (ret);
}

bool ibd2sdi::create_sql_from_copy(ib_tablespace *ts, const char *ibd_path) {
  page_no_t root_page_num = ts->get_sdi_root();
  page_size_t page_size(ts->get_page_size());

  byte buf_unalign[2 * UNIV_PAGE_SIZE_MAX];
  byte *buf = static_cast<byte *>(ut_align(buf_unalign, page_size.logical()));
  memset(buf, 0, page_size.logical());

  switch (reach_to_leftmost_leaf_level(ts, page_size.logical(), buf,
                                       root_page_num)) {
    case SUCCESS:
      break;
    case FALIURE:
      return true;
    case NO_RECORDS:
      ib::info() << "SDI is empty";
      return false;
    default:
      ut_ad(0);
  }

  byte *current_rec = get_first_user_rec(ts, page_size.logical(), buf);
  uint64_t sdi_id;
  uint64_t sdi_type;
  byte *sdi_data = nullptr;
  uint64_t sdi_data_len = 0;
  bool corrupt = false;

  std::string accumulated_sql;

  while (current_rec != nullptr &&
         get_rec_type(current_rec) != REC_STATUS_SUPREMUM && !corrupt) {
    dberr_t err = parse_fields_in_rec(ts, current_rec, &sdi_type, &sdi_id,
                                      &sdi_data, &sdi_data_len);

    if (err == DB_SUCCESS && sdi_data != nullptr) {
      create_sql::process_sdi_create_sql(
          reinterpret_cast<const char *>(sdi_data), sdi_data_len, ibd_path,
          accumulated_sql);
      free(sdi_data);
      sdi_data = nullptr;
    }

    current_rec =
        get_next_rec(ts, current_rec, page_size.logical(), buf, &corrupt);
  }

  if (!accumulated_sql.empty()) {
    create_sql::write_sql_output(accumulated_sql, ibd_path);
  }

  return corrupt;
}

bool ibd2sdi::dump_create_sql(const char *ibd_path) {
  ut_a(m_tablespace_creator != nullptr);
  ib_tablespace *ts = m_tablespace_creator->get_tablespace();
  return create_sql_from_copy(ts, ibd_path);
}

int main(int argc, char **argv) {
  /* Buffer to hold temporary file name. */
  char tmp_filename_buf[FN_REFLEN];
  FILE *dump_file;
  bool ret = true;

  ut_crc32_init();
  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  if (get_options(&argc, &argv)) {
    return 1;
  }

  if (opts.no_checksum &&
      srv_checksum_algorithm != SRV_CHECKSUM_ALGORITHM_INNODB) {
    ib::error() << "Invalid combination of options. Cannot use"
                   " --no-check & --strict-check together";
    return 1;
  }

  if (opts.is_sdi_id && opts.is_sdi_type) {
    opts.is_sdi_rec = true;
  }

  if (opts.is_dump_file) {
    char dir_buf[FN_REFLEN];
    size_t dir_length;
    memset(dir_buf, 0, FN_REFLEN);
    if (dirname_part(dir_buf, opts.dump_filename, &dir_length) == 0) {
      sprintf(dir_buf, "./");
    }

    dump_file = create_tmp_file(tmp_filename_buf, dir_buf, "ib_sdi");

    if (dump_file == nullptr) {
      ib::error() << "Invalid Dumpfile passed";
      return 1;
    }
  } else {
    dump_file = stdout;
  }

  /* --create-sql forces data reading (cannot skip) */
  bool skip_data = opts.create_sql ? false : opts.skip_data;
  ibd2sdi sdi(argc, argv, dump_file, skip_data);

  if (sdi.process_files()) {
    return 1;
  }

  if (opts.create_sql) {
    ret = sdi.dump_create_sql(argv[0]);
  } else if (opts.is_sdi_rec) {
    ret = sdi.dump_one_sdi(opts.sdi_rec_id, opts.sdi_rec_type);
  } else if (opts.is_sdi_id) {
    ret = sdi.dump_matching_ids(opts.sdi_rec_id);
  } else if (opts.is_sdi_type) {
    ret = sdi.dump_matching_types(opts.sdi_rec_type);
  } else {
    ret = sdi.dump();
  }

  if (!ret && opts.is_dump_file) {
    /* Rename file can fail if the source and destination
    are across partitions. */
    if (my_rename(tmp_filename_buf, opts.dump_filename, MYF(0)) == -1) {
      if (my_copy(tmp_filename_buf, opts.dump_filename, MYF(0)) != 0) {
        ib::error() << "Copy failed: from: " << tmp_filename_buf
                    << " to: " << opts.dump_filename
                    << " because of system error: " << strerror(errno);

        ib::error() << "Please check contents of"
                    << " temporary file " << tmp_filename_buf
                    << " and delete it manually";
        return 1;
      }
      try_delete_temporary_filename(tmp_filename_buf);
    }
  } else if (opts.is_dump_file) {
    try_delete_temporary_filename(tmp_filename_buf);
  }

  return ret ? 1 : 0;
}
