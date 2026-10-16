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

#include "restore_filter.h"

#include <cctype>
#include <cstring>
#include <fstream>

#include "my_inttypes.h"
#include "mysql/strings/m_ctype.h"

namespace xb {
namespace restore {

/** Compare two bytes, ignoring ASCII case when asked to.
@param[in]	a			first byte
@param[in]	b			second byte
@param[in]	case_insensitive	ignore ASCII case
@return true if equal */
static bool char_equal(char a, char b, bool case_insensitive) {
  if (case_insensitive) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  }
  return a == b;
}

bool glob_match(const std::string &pattern, const std::string &text,
                bool case_insensitive) {
  size_t p = 0;
  size_t t = 0;
  /* Position after the last '*' seen and the text position it was tried at,
  for backtracking. */
  size_t star_p = std::string::npos;
  size_t star_t = 0;

  while (t < text.size()) {
    if (p < pattern.size()) {
      char c = pattern[p];
      if (c == '*') {
        star_p = ++p;
        star_t = t;
        continue;
      }
      if (c == '?') {
        ++p;
        ++t;
        continue;
      }
      if (c == '\\' && p + 1 < pattern.size()) {
        c = pattern[p + 1];
        if (char_equal(c, text[t], case_insensitive)) {
          p += 2;
          ++t;
          continue;
        }
      } else if (char_equal(c, text[t], case_insensitive)) {
        ++p;
        ++t;
        continue;
      }
    }
    if (star_p == std::string::npos) {
      return false;
    }
    /* Let the last '*' swallow one more character and try again. */
    p = star_p;
    t = ++star_t;
  }

  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

/** Append a Unicode code point to a string as UTF-8.
@param[in]	wc	code point
@param[in,out]	out	string to append to */
static void append_utf8(my_wc_t wc, std::string *out) {
  if (wc < 0x80) {
    out->push_back(static_cast<char>(wc));
  } else if (wc < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (wc >> 6)));
    out->push_back(static_cast<char>(0x80 | (wc & 0x3F)));
  } else if (wc < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (wc >> 12)));
    out->push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (wc & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (wc >> 18)));
    out->push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (wc & 0x3F)));
  }
}

bool decode_fs_name(const std::string &fs_name, std::string *name) {
  const CHARSET_INFO *cs = &my_charset_filename;
  const auto *s = reinterpret_cast<const uchar *>(fs_name.data());
  const uchar *e = s + fs_name.size();

  name->clear();
  while (s < e) {
    my_wc_t wc;
    const int len = cs->cset->mb_wc(cs, &wc, s, e);
    if (len <= 0) {
      return false;
    }
    append_utf8(wc, name);
    s += len;
  }
  return true;
}

/** Whether a string ends with a suffix.
@param[in]	s	string
@param[in]	suffix	suffix
@return true if it does */
static bool ends_with(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** Remove the compression and encryption suffixes a backup adds to a file.
@param[in,out]	name	file name */
static void strip_transform_suffixes(std::string *name) {
  static const char *suffixes[] = {".xbcrypt", ".zst", ".lz4", ".qp"};
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (const char *suffix : suffixes) {
      if (ends_with(*name, suffix)) {
        name->resize(name->size() - strlen(suffix));
        stripped = true;
      }
    }
  }
}

/** Whether a table name is that of a FULLTEXT auxiliary table,
fts_<16 hex digits>_...
@param[in]	name	table name, encoded
@return true if it is */
static bool is_fts_aux_name(const std::string &name) {
  if (name.size() < 21 ||
      (name.compare(0, 4, "fts_") != 0 && name.compare(0, 4, "FTS_") != 0)) {
    return false;
  }
  for (size_t i = 4; i < 20; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) {
      return false;
    }
  }
  return name[20] == '_';
}

bool parse_table_file(const std::string &path, std::string *fs_db,
                      std::string *fs_table) {
  const size_t slash = path.find('/');
  /* Tables live in a database directory directly below the backup root. */
  if (slash == std::string::npos || slash == 0 ||
      path.find('/', slash + 1) != std::string::npos) {
    return false;
  }

  std::string file = path.substr(slash + 1);
  strip_transform_suffixes(&file);

  static const char *kinds[] = {
      ".ibd.new.delta", ".ibd.new.meta", ".ibd.new", ".ibd.delta",
      ".ibd.meta",      ".ibd.crpt",     ".ibd"};
  bool found = false;
  for (const char *kind : kinds) {
    if (ends_with(file, kind)) {
      file.resize(file.size() - strlen(kind));
      found = true;
      break;
    }
  }
  if (!found || file.empty()) {
    return false;
  }

  /* Intermediate tables of ALTER TABLE and FULLTEXT auxiliary tables are not
  tables a user can name. */
  if (file.compare(0, 4, "#sql") == 0 || is_fts_aux_name(file)) {
    return false;
  }

  /* A partition is "<table>#p#<partition>" and a subpartition adds
  "#sp#<subpartition>"; servers before 8.0.17 wrote them in upper case. */
  size_t hash = std::string::npos;
  for (size_t i = 0; i + 3 <= file.size(); ++i) {
    if (file[i] == '#' && (file[i + 1] == 'p' || file[i + 1] == 'P') &&
        file[i + 2] == '#') {
      hash = i;
      break;
    }
  }
  if (hash != std::string::npos) {
    file.resize(hash);
  }
  if (file.empty() || file.find('#') != std::string::npos) {
    return false;
  }

  *fs_db = path.substr(0, slash);
  *fs_table = file;
  return true;
}

bool is_core_file(const std::string &path) {
  const size_t slash = path.find('/');
  std::string file = path;
  strip_transform_suffixes(&file);

  if (slash != std::string::npos) {
    /* Inside a database directory only the reduced-lock rename markers are
    needed: they name tables by space id, so they can't be matched to one. */
    return path.find('/', slash + 1) == std::string::npos &&
           ends_with(file, ".ren");
  }

  /* At the root, everything but general tablespaces: mysql.ibd holds the data
  dictionary and is always needed. */
  if (ends_with(file, ".ibd") || ends_with(file, ".ibd.delta") ||
      ends_with(file, ".ibd.meta")) {
    return file.compare(0, 9, "mysql.ibd") == 0;
  }
  return true;
}

bool Table_filter::add_one(const std::string &text, bool include,
                           std::string *error) {
  /* Split at the first dot that a backslash does not escape. */
  size_t dot = std::string::npos;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\') {
      ++i;
    } else if (text[i] == '.') {
      dot = i;
      break;
    }
  }

  Pattern p;
  p.text = text;
  if (dot == std::string::npos) {
    p.db = text;
    p.table = "*";
  } else {
    p.db = text.substr(0, dot);
    p.table = text.substr(dot + 1);
  }

  if (p.db.empty() || p.table.empty()) {
    *error = "invalid pattern '" + text + "': expected db or db.table";
    return false;
  }

  (include ? m_include : m_exclude).push_back(p);
  return true;
}

/** Remove leading and trailing blanks.
@param[in]	s	string
@return trimmed copy */
static std::string trim(const std::string &s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return "";
  }
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool Table_filter::add_list(const std::string &list, bool include,
                            std::string *error) {
  size_t start = 0;
  while (start <= list.size()) {
    size_t comma = list.find(',', start);
    if (comma == std::string::npos) {
      comma = list.size();
    }
    const std::string item = trim(list.substr(start, comma - start));
    if (!item.empty() && !add_one(item, include, error)) {
      return false;
    }
    start = comma + 1;
  }
  return true;
}

bool Table_filter::add_file(const std::string &path, bool include,
                            std::string *error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot open pattern file '" + path + "'";
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (!add_one(line, include, error)) {
      return false;
    }
  }
  return true;
}

bool Table_filter::matches(const Pattern &p, const std::string &db,
                           const std::string &table) const {
  return glob_match(p.db, db, m_case_insensitive) &&
         glob_match(p.table, table, m_case_insensitive);
}

bool Table_filter::select(const std::string &db, const std::string &table) {
  bool included = false;
  for (auto &p : m_include) {
    if (matches(p, db, table)) {
      p.used = true;
      included = true;
    }
  }
  if (!included) {
    return false;
  }
  for (const auto &p : m_exclude) {
    if (matches(p, db, table)) {
      return false;
    }
  }
  return true;
}

bool Table_filter::select_fs(const std::string &fs_db,
                             const std::string &fs_table) {
  std::string db;
  std::string table;
  if (!decode_fs_name(fs_db, &db) || !decode_fs_name(fs_table, &table)) {
    return false;
  }
  return select(db, table);
}

std::vector<std::string> Table_filter::unused_includes() const {
  std::vector<std::string> unused;
  for (const auto &p : m_include) {
    if (!p.used) {
      unused.push_back(p.text);
    }
  }
  return unused;
}

}  // namespace restore
}  // namespace xb
