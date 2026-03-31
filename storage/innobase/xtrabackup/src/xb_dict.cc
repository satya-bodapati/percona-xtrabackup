/******************************************************
Copyright (c) 2023 Percona LLC and/or its affiliates.

interface containing map/set required for PXB

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
#include "xb_dict.h"
#include <dd/properties.h>
#include <sql_class.h>
#include <fstream>
#include <memory>
#include <unordered_map>
#include "backup_mysql.h"
#include "include/api0api.h"
#include "include/api0misc.h"
#include "include/dict0sdi-decompress.h"
#include "sql/current_thd.h"
#include "sql/dd/dd.h"
#include "sql/dd/dd_schema.h"  // dd::Schema_MDL_locker
#include "sql/dd/dd_table.h"   // is_encrypted
#include "sql/dd/impl/sdi.h"
#include "sql/dd/impl/tables/dd_properties.h"  // dd::tables:.DD_properties
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/table_impl.h"
#include "sql/dd/properties.h"  // dd::Properties
#include "sql/dd/string_type.h"
#include "sql/dd/types/check_constraint.h"  // dd::Check_constraint
#include "sql/dd/types/column.h"            // dd::Column
#include "sql/dd/types/column_type_element.h"
#include "sql/dd/types/foreign_key.h"          // dd::Foreign_key
#include "sql/dd/types/foreign_key_element.h"  // dd::Foreign_key_element
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/partition_value.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/table.h"  // dd::Table
#include "storage/innobase/include/btr0pcur.h"
#include "storage/innobase/include/dict0dd.h"
#include "xb0xb.h"
#include "xtrabackup.h"

namespace xb {
// Dictionary used by backup phase. Currently we query running server to know
// the list of tablespaces. PXB currently uses *.ibd scan to find the
// tablespaces We use the dictionary during backup phase to detect the "orphan"
// IBDs. i.e. the IBDs found in data directory but doesn't have any entry in
// server dictionary.
namespace backup {
std::shared_ptr<dd_space_ids> build_space_id_set(MYSQL *connection) {
  ut_ad(srv_backup_mode);

  std::shared_ptr<dd_space_ids> dd_tab = std::make_shared<dd_space_ids>();
  std::string sql = "SELECT SPACE FROM INFORMATION_SCHEMA.INNODB_TABLESPACES ";

  MYSQL_RES *result = xb_mysql_query(connection, sql.c_str(), true, true);
  if (result) {
    auto rows_counts = mysql_num_rows(result);
    if (rows_counts > 0) {
      MYSQL_ROW row;
      while ((row = mysql_fetch_row(result)) != nullptr) {
        space_id_t space_id = atoi(row[0]);
        dd_tab->insert(space_id);
      }
    } else {
      xb::warn() << " Query " << sql << " did not return any value ";
      return nullptr;
    }
    mysql_free_result(result);
  } else {
    xb::warn() << "Failed to execute query " << sql;
    return nullptr;
  }

  return dd_tab;
}
}  // namespace backup

namespace prepare {
/** map of <table_id, space_id> */
static std::unordered_map<table_id_t, space_id_t> table_id_space_map;

/** map of <space_id, partition_index_id>
Used for answering: Given a space_id, which partition table does it belong
If space_id doesn't exist in this map, it means the table is not partitioned */
static std::unordered_map<space_id_t, uint64_t> space_part_map;

/** multimap (duplicates allowed) of partition_index_id and space_id.
A partition table can contain many space_ids */
static std::multimap<uint64_t, space_id_t> part_id_spaces_map;

/* map of <schema id, name> and SDI id
This map is used to handle duplicate SDI */
static std::map<std::pair<int, std::string>, uint64> sdi_id_map;

/* map of schema name and schema id
This map is used to handle duplicate SDI */
static std::map<std::string, uint64> dd_schema_map;

using dd_Table_Ptr = std::unique_ptr<dd::Table>;

/** @return true if table_id is found in dd map. This map
is created by scanning mysql.indexes and mysql.index_partitions
@param[in]  table_id InnoDB table id */
bool table_exists_in_dd(table_id_t table_id) {
  return (table_id_space_map.find(table_id) != table_id_space_map.end());
}

/** @return true if tablespace belongs to a partition
@param[in] space_id InnoDB tablespace id */
static bool is_space_partitioned(space_id_t space_id) {
  return (space_part_map.find(space_id) != space_part_map.end());
}

/** @return get partition id for a tablespace. If space is not partitioned,
return 0. All tablespace partitions of a single table have same partition id
@param[in] space_id InnoDB tablespace id */
static uint64_t get_part_id_for_space(space_id_t space_id) {
  ut_ad(is_space_partitioned(space_id));
  auto it = space_part_map.find(space_id);
  return (it != space_part_map.end() ? it->second : 0);
}

/** Scan the SDI id from DD table "mysql.tables"
@param[in]  name       tablespace name database/name
@param[out] sdi_id     id of table
@param[out] table_name name of the table
@param[in]  thd        THD
@return DB_SUCCESS on success, other DB_* on error */
static dberr_t get_sdi_id_from_dd(const std::string &name, uint64 *sdi_id,
                                  std::string &table_name, THD *thd) {
  ut_ad(!dict_sys_mutex_own());

  std::string db_name;
  uint64 schema_id = 0;

  /* get the database and table_name from space name */
  dict_name::get_table(name, db_name, table_name);

  ut_ad(db_name.compare("mysql") != 0);

  /* map of schema name and id built from scanning mysql/schemata and map of
  <schema id, name> and SDI id built from scanning mysql/tables */
  if (dd_schema_map.size() == 0 && sdi_id_map.size() == 0) {
    dict_table_t *sys_tables = nullptr;
    btr_pcur_t pcur;
    const rec_t *rec = nullptr;
    mtr_t mtr;
    MDL_ticket *mdl = nullptr;
    mem_heap_t *heap = mem_heap_create(1000, UT_LOCATION_HERE);

    dict_sys_mutex_enter();
    mtr_start(&mtr);
    rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, "mysql/schemata",
                              &sys_tables);
    while (rec) {
      uint64 rec_schema_id;
      std::string rec_name;

      dd_process_schema_rec(heap, rec, sys_tables, &mtr, &rec_name,
                            &rec_schema_id);
      dd_schema_map.insert(std::make_pair(rec_name, rec_schema_id));
      mem_heap_empty(heap);

      mtr_start(&mtr);
      rec = (rec_t *)dd_getnext_system_rec(&pcur, &mtr);
    }

    mtr_commit(&mtr);
    dd_table_close(sys_tables, thd, &mdl, true);
    mem_heap_empty(heap);

    mtr_start(&mtr);

    rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, "mysql/tables",
                              &sys_tables);

    while (rec) {
      uint64 rec_schema_id;
      std::string rec_name;
      uint64 rec_id;

      dd_process_dd_tables_rec(heap, rec, sys_tables, &mtr, &rec_schema_id,
                               &rec_name, &rec_id);
      mem_heap_empty(heap);

      auto rec_table_id = std::make_pair(rec_schema_id, rec_name);

      sdi_id_map.insert(
          std::make_pair(std::make_pair(rec_schema_id, rec_name), rec_id));

      mtr_start(&mtr);
      rec = (rec_t *)dd_getnext_system_rec(&pcur, &mtr);
    }

    mtr_commit(&mtr);
    dd_table_close(sys_tables, thd, &mdl, true);
    mem_heap_free(heap);

    dict_sys_mutex_exit();
  }

  auto it = dd_schema_map.find(db_name);

  if (it == dd_schema_map.end()) {
    xb::error() << "can't find " << db_name.c_str()
                << " entry in mysql/schemata for tablespace " << name.c_str();
    return (DB_NOT_FOUND);
  } else {
    schema_id = it->second;
    ut_ad(schema_id != 0);
  }

  auto it2 = sdi_id_map.find(std::make_pair(schema_id, table_name));
  if (it2 == sdi_id_map.end()) {
    xb::error() << "can't find " << table_name.c_str()
                << " entry in mysql/tables for tablespace " << name.c_str();
    return (DB_NOT_FOUND);
  } else {
    *sdi_id = it2->second;
    ut_ad(*sdi_id != 0);
  }
  return (DB_SUCCESS);
}

/** Load a specific table from space_id
@param[in] space_id InnoDB tablespace_id
@param[in] table_id InnoDB table id
@return tuple <a,b>
a - DB_SUCCESS on success, other DB_ on errors
b - std::vector<dict_table_t*>, empty on errors */
static xb_dict_tuple dict_load_tables_from_space_id_wrapper(
    space_id_t space_id, table_id_t table_id) {
  fil_space_t *space = fil_space_get(space_id);

  if (space == nullptr) {
    dberr_t err = fil_xb_get_tablespace_error(space_id);
    /* Check if tablespace errored out when it was loaded. If yes, we abort
    the prepare. If no, we consider the tablespace as dropped */
    if (err != DB_ERROR_UNSET) {
      std::string tablespace_name;
      bool found = fil_system_get_file_by_space_id(space_id, tablespace_name);
      xb::error() << "Tablespace " << (found ? tablespace_name : "")
                  << " with space_id " << space_id
                  << " is required for transaction rollback but it was not"
                  << " loaded because of DB_ error " << err;

      if (err == DB_INVALID_ENCRYPTION_META) {
        xb::error() << KEYRING_NOT_LOADED;
      }

      exit(EXIT_FAILURE);
    }

    return {DB_TABLESPACE_NOT_FOUND, {}};
  }

  THD *thd = current_thd;
  ut_a(thd != nullptr);
  ib_trx_t trx = ib_trx_begin(IB_TRX_READ_COMMITTED, false, false, thd);

  auto result = dict_load_tables_from_space_id(space_id, table_id, thd, trx);

  ib_trx_commit(trx);
  ib_trx_release(trx);

  return result;
}

/** Load a specific table from space_id. This is used by InnoDB table opening
function dd_table_open_on_id()
@param[in] table_id InnoDB table id
@return tuple <a,b>
a - DB_SUCCESS on success, other DB_ on errors
b - std::vector<dict_table_t*>, empty on errors */
xb_dict_tuple dict_load_tables_using_table_id(table_id_t table_id) {
  auto it = table_id_space_map.find(table_id);
  if (it == table_id_space_map.end()) {
    // Table_id not present in any space_id. A dropped table
    return {DB_TABLESPACE_NOT_FOUND, {}};
  }

  space_id_t space_id = it->second;
  DBUG_LOG("xb_dd",
           "space_id is " << space_id << " for table_id: " << table_id);

  if (!is_space_partitioned(space_id)) {
    return (dict_load_tables_from_space_id_wrapper(space_id, table_id));
  }

  // For partition tables, SDI exists in only one partition IBD, loop
  // through such IBDs to find the required table_id
  uint64_t part_id = get_part_id_for_space(space_id);
  if (part_id == 0) return {DB_TABLESPACE_NOT_FOUND, {}};

  auto low = part_id_spaces_map.lower_bound(part_id);
  auto high = part_id_spaces_map.upper_bound(part_id);
  while (low != high) {
    space_id_t space_id = low->second;
    auto result = dict_load_tables_from_space_id_wrapper(space_id, table_id);
    // Look for the desired table_id in the result tables
    dberr_t err = std::get<0>(result);
    auto tables_vec = std::get<1>(result);
    if (err != DB_SUCCESS) {
      // Possibly we are in partition IBD that doesn't have SDI, keep
      // looking other partition space_ids
      ++low;
      continue;
    }
    auto end = tables_vec.end();

    auto i = std::search_n(tables_vec.begin(), end, 1, table_id,
                           [](const dict_table_t *table, table_id_t id) {
                             return (table->id == id);
                           });
    if (i == end) {
      // Possibly we are in partition IBD that doesn't have SDI, keep
      // looking other partition space_ids
      ++low;
      continue;

    } else {
      // Found desired table
      return (result);
    }
    ++low;
  }
  return {DB_ERROR, {}};
}

static bool has_default(dd::Column *dd_col) {
  return !dd_col->has_no_default() && !dd_col->is_auto_increment();
}

static std::string escape_string(const dd::String_type &s) {
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

static std::string unescape_dd_string(const dd::String_type &s) {
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

static std::string quote_identifier(const dd::String_type &name) {
  std::string r = "`";
  for (char c : name) {
    if (c == '`') r += '`';
    r += c;
  }
  r += '`';
  return r;
}

static const char *fk_rule_str(dd::Foreign_key::enum_rule rule) {
  switch (rule) {
    case dd::Foreign_key::RULE_RESTRICT:
      return "RESTRICT";
    case dd::Foreign_key::RULE_CASCADE:
      return "CASCADE";
    case dd::Foreign_key::RULE_SET_NULL:
      return "SET NULL";
    case dd::Foreign_key::RULE_SET_DEFAULT:
      return "SET DEFAULT";
    default:
      return nullptr;
  }
}

static void show_create_table(space_id_t space_id, dd::Table *dd_table,
                              const dd::String_type &schema_name) {
  if (space_id == dict_sys_t::s_dict_space_id) {
    return;
  }

  dd::Table &tbl = *dd_table;
  std::ostringstream ss;
  bool first_col = true;

  ss << "CREATE TABLE " << quote_identifier(tbl.name()) << " (\n";

  for (const auto col : *tbl.columns()) {
    if (col->is_se_hidden() ||
        col->hidden() == dd::Column::enum_hidden_type::HT_HIDDEN_SQL)
      continue;

    if (!first_col) ss << ",\n";
    first_col = false;

    ss << "  " << quote_identifier(col->name()) << " "
       << col->column_type_utf8();

    if (col->is_explicit_collation()) {
      const CHARSET_INFO *col_cs = dd_get_mysql_charset(col->collation_id());
      if (col_cs && strcmp(col_cs->csname, "binary") != 0 &&
          col->type() != dd::enum_column_types::GEOMETRY) {
        ss << " CHARACTER SET " << col_cs->csname;
        ss << " COLLATE " << col_cs->m_coll_name;
      }
    }

    bool is_gcol = !col->is_generation_expression_utf8_null();
    if (is_gcol) {
      ss << " GENERATED ALWAYS AS (" << col->generation_expression_utf8()
         << ")";
      if (col->is_virtual())
        ss << " VIRTUAL";
      else
        ss << " STORED";
    }

    if (!col->is_nullable()) {
      ss << " NOT NULL";
    } else if (col->type() == dd::enum_column_types::TIMESTAMP ||
               col->type() == dd::enum_column_types::TIMESTAMP2) {
      ss << " NULL";
    }

    const auto &col_opts = col->options();
    uint64 not_secondary_val;
    if (col_opts.exists("not_secondary") &&
        !col_opts.get("not_secondary", &not_secondary_val) &&
        not_secondary_val) {
      ss << " NOT SECONDARY";
    }

    if (col->type() == dd::enum_column_types::GEOMETRY && col->srs_id()) {
      ss << " /*!80003 SRID " << col->srs_id().value() << " */";
    }

    if (!is_gcol && has_default(col)) {
      ss << " DEFAULT";
      if (!col->default_option().empty()) {
        const auto &opt = col->default_option();
        if (opt.find("CURRENT_TIMESTAMP") == 0) {
          ss << " " << opt;
        } else {
          ss << " (" << opt << ")";
        }
      } else if (col->is_default_value_utf8_null()) {
        ss << " NULL";
      } else {
        const auto &val = col->default_value_utf8();
        bool needs_quote = true;
        switch (col->type()) {
          case dd::enum_column_types::DECIMAL:
          case dd::enum_column_types::NEWDECIMAL:
          case dd::enum_column_types::TINY:
          case dd::enum_column_types::SHORT:
          case dd::enum_column_types::LONG:
          case dd::enum_column_types::LONGLONG:
          case dd::enum_column_types::INT24:
          case dd::enum_column_types::FLOAT:
          case dd::enum_column_types::DOUBLE:
          case dd::enum_column_types::BIT:
            needs_quote = false;
            break;
          default:
            break;
        }
        if (needs_quote) {
          ss << " '" << escape_string(val) << "'";
        } else {
          ss << " " << val;
        }
      }
    }

    if (!col->update_option().empty()) {
      ss << " ON UPDATE " << col->update_option();
    }

    if (col->is_auto_increment()) {
      ss << " AUTO_INCREMENT";
    }

    if (col->hidden() == dd::Column::enum_hidden_type::HT_HIDDEN_USER) {
      ss << " /*!80023 INVISIBLE */";
    }

    if (!col->comment().empty()) {
      ss << " COMMENT '" << escape_string(col->comment()) << "'";
    }

    auto col_ea = col->engine_attribute();
    if (col_ea.length > 0) {
      ss << " /*!80021 ENGINE_ATTRIBUTE '"
         << escape_string(dd::String_type(col_ea.str, col_ea.length)) << "' */";
    }

    auto col_sea = col->secondary_engine_attribute();
    if (col_sea.length > 0) {
      ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE '"
         << escape_string(dd::String_type(col_sea.str, col_sea.length))
         << "' */";
    }
  }

  for (const auto key : *tbl.indexes()) {
    if (key->is_hidden()) continue;

    bool found_primary = false;
    ss << ",\n  ";
    if (key->type() == dd::Index::IT_PRIMARY) {
      found_primary = true;
      ss << "PRIMARY KEY";
    } else if (key->type() == dd::Index::IT_UNIQUE) {
      ss << "UNIQUE KEY ";
    } else if (key->type() == dd::Index::IT_FULLTEXT) {
      ss << "FULLTEXT KEY ";
    } else if (key->type() == dd::Index::IT_SPATIAL) {
      ss << "SPATIAL KEY ";
    } else if (key->type() == dd::Index::IT_MULTIPLE) {
      ss << "KEY ";
    }

    if (!found_primary) {
      ss << quote_identifier(key->name());
    }

    ss << " (";

    bool first_elem = true;
    for (const auto key_elem : key->elements()) {
      if (key_elem->is_hidden()) continue;

      if (!first_elem) ss << ",";
      first_elem = false;

      const auto &elem_col = key_elem->column();
      if (elem_col.hidden() == dd::Column::enum_hidden_type::HT_HIDDEN_SQL) {
        ss << "(" << elem_col.generation_expression_utf8() << ")";
      } else {
        ss << quote_identifier(elem_col.name());
        if (!key_elem->is_length_null() && key_elem->is_prefix() &&
            key->type() != dd::Index::IT_FULLTEXT &&
            key->type() != dd::Index::IT_SPATIAL) {
          uint prefix_len = key_elem->length();
          const CHARSET_INFO *elem_cs =
              dd_get_mysql_charset(elem_col.collation_id());
          if (elem_cs && elem_cs->mbmaxlen > 0) {
            prefix_len /= elem_cs->mbmaxlen;
          }
          ss << "(" << prefix_len << ")";
        }
        if (key_elem->order() == dd::Index_element::ORDER_DESC) {
          ss << " DESC";
        }
      }
    }

    ss << ")";

    if (key->is_algorithm_explicit()) {
      switch (key->algorithm()) {
        case dd::Index::IA_BTREE:
          ss << " USING BTREE";
          break;
        case dd::Index::IA_HASH:
          ss << " USING HASH";
          break;
        case dd::Index::IA_RTREE:
          if (key->type() != dd::Index::IT_SPATIAL) ss << " USING RTREE";
          break;
        default:
          break;
      }
    }

    const auto &key_opts = key->options();
    uint64 idx_block_size;
    if (key_opts.exists("block_size") &&
        !key_opts.get("block_size", &idx_block_size) && idx_block_size != 0) {
      ss << " KEY_BLOCK_SIZE=" << idx_block_size;
    }

    if (!key->comment().empty()) {
      ss << " COMMENT '" << escape_string(key->comment()) << "'";
    }

    if (!key->is_visible()) {
      ss << " /*!80000 INVISIBLE */";
    }

    auto idx_ea = key->engine_attribute();
    if (idx_ea.length > 0) {
      ss << " /*!80021 ENGINE_ATTRIBUTE '"
         << escape_string(dd::String_type(idx_ea.str, idx_ea.length)) << "' */";
    }

    auto idx_sea = key->secondary_engine_attribute();
    if (idx_sea.length > 0) {
      ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE '"
         << escape_string(dd::String_type(idx_sea.str, idx_sea.length))
         << "' */";
    }

    dd::String_type parser_name;
    if (key_opts.exists("parser_name") &&
        !key_opts.get("parser_name", &parser_name) && !parser_name.empty()) {
      ss << " /*!50100 WITH PARSER " << quote_identifier(parser_name) << " */";
    }
  }

  for (const auto fk : *tbl.foreign_keys()) {
    ss << ",\n  CONSTRAINT " << quote_identifier(fk->name())
       << " FOREIGN KEY (";
    bool first_fk_col = true;
    for (const auto fk_el : *fk->elements()) {
      if (!first_fk_col) ss << ",";
      first_fk_col = false;
      ss << quote_identifier(fk_el->column().name());
    }
    ss << ") REFERENCES " << quote_identifier(fk->referenced_table_name())
       << " (";
    bool first_ref_col = true;
    for (const auto fk_el : *fk->elements()) {
      if (!first_ref_col) ss << ",";
      first_ref_col = false;
      ss << quote_identifier(fk_el->referenced_column_name());
    }
    ss << ")";
    const char *del_rule = fk_rule_str(fk->delete_rule());
    if (del_rule) ss << " ON DELETE " << del_rule;
    const char *upd_rule = fk_rule_str(fk->update_rule());
    if (upd_rule) ss << " ON UPDATE " << upd_rule;
  }

  for (const auto cc : *tbl.check_constraints()) {
    ss << ",\n  CONSTRAINT " << quote_identifier(cc->name()) << " CHECK ("
       << unescape_dd_string(cc->check_clause_utf8()) << ")";
    if (cc->constraint_state() == dd::Check_constraint::CS_NOT_ENFORCED) {
      ss << " /*!80016 NOT ENFORCED */";
    }
  }

  ss << "\n)";

  const auto &opts = tbl.options();

  ss << " ENGINE=" << tbl.engine().c_str();

  /* DEFAULT CHARSET / COLLATE */
  const CHARSET_INFO *cs = dd_get_mysql_charset(tbl.collation_id());
  ss << " DEFAULT CHARSET=" << cs->csname;
  if (!(cs->state & MY_CS_PRIMARY) || cs == &my_charset_utf8mb4_0900_ai_ci) {
    ss << " COLLATE=" << cs->m_coll_name;
  }

  /* PACK_KEYS */
  uint64 pack_keys_val;
  if (opts.exists("pack_keys") && !opts.get("pack_keys", &pack_keys_val)) {
    ss << " PACK_KEYS=" << (pack_keys_val ? "1" : "0");
  }

  /* STATS_PERSISTENT */
  uint64 stats_persistent_val;
  if (opts.exists("stats_persistent") &&
      !opts.get("stats_persistent", &stats_persistent_val)) {
    ss << " STATS_PERSISTENT=" << (stats_persistent_val ? "1" : "0");
  }

  /* STATS_AUTO_RECALC */
  uint64 stats_auto_recalc_val;
  if (opts.exists("stats_auto_recalc") &&
      !opts.get("stats_auto_recalc", &stats_auto_recalc_val)) {
    if (stats_auto_recalc_val == 1)
      ss << " STATS_AUTO_RECALC=1";
    else if (stats_auto_recalc_val == 2)
      ss << " STATS_AUTO_RECALC=0";
  }

  /* STATS_SAMPLE_PAGES */
  uint64 stats_sample_pages_val;
  if (opts.exists("stats_sample_pages") &&
      !opts.get("stats_sample_pages", &stats_sample_pages_val) &&
      stats_sample_pages_val != 0) {
    ss << " STATS_SAMPLE_PAGES=" << stats_sample_pages_val;
  }

  /* ROW_FORMAT */
  switch (tbl.row_format()) {
    case dd::Table::RF_COMPRESSED:
      ss << " ROW_FORMAT=COMPRESSED";
      break;
    case dd::Table::RF_REDUNDANT:
      ss << " ROW_FORMAT=REDUNDANT";
      break;
    case dd::Table::RF_COMPACT:
      ss << " ROW_FORMAT=COMPACT";
      break;
    default:
      break;
  }

  /* KEY_BLOCK_SIZE */
  uint64 key_block_size_val;
  if (opts.exists("key_block_size") &&
      !opts.get("key_block_size", &key_block_size_val) &&
      key_block_size_val != 0) {
    ss << " KEY_BLOCK_SIZE=" << key_block_size_val;
  }

  /* COMPRESSION */
  dd::String_type compress_val;
  if (opts.exists("compress") && !opts.get("compress", &compress_val) &&
      !compress_val.empty()) {
    ss << " COMPRESSION='" << compress_val << "'";
  }

  /* ENCRYPTION */
  dd::String_type encrypt_val;
  if (opts.exists("encrypt_type") && !opts.get("encrypt_type", &encrypt_val) &&
      !encrypt_val.empty() && encrypt_val != "N") {
    ss << " ENCRYPTION='" << encrypt_val << "'";
  }

  /* COMMENT */
  if (!tbl.comment().empty()) {
    ss << " COMMENT='" << escape_string(tbl.comment()) << "'";
  }

  /* ENGINE_ATTRIBUTE */
  auto ea = tbl.engine_attribute();
  if (ea.length > 0) {
    ss << " /*!80021 ENGINE_ATTRIBUTE='"
       << escape_string(dd::String_type(ea.str, ea.length)) << "' */";
  }

  /* SECONDARY_ENGINE_ATTRIBUTE */
  auto sea = tbl.secondary_engine_attribute();
  if (sea.length > 0) {
    ss << " /*!80021 SECONDARY_ENGINE_ATTRIBUTE='"
       << escape_string(dd::String_type(sea.str, sea.length)) << "' */";
  }

  if (tbl.partition_type() != dd::Table::PT_NONE) {
    ss << "\n/*!50100 PARTITION BY ";
    const auto &part_expr = tbl.partition_expression_utf8();
    switch (tbl.partition_type()) {
      case dd::Table::PT_HASH:
        ss << "HASH (" << part_expr << ")";
        break;
      case dd::Table::PT_LINEAR_HASH:
        ss << "LINEAR HASH (" << part_expr << ")";
        break;
      case dd::Table::PT_KEY_55:
      case dd::Table::PT_KEY_51:
        ss << "KEY (" << part_expr << ")";
        break;
      case dd::Table::PT_LINEAR_KEY_51:
      case dd::Table::PT_LINEAR_KEY_55:
        ss << "LINEAR KEY (" << part_expr << ")";
        break;
      case dd::Table::PT_RANGE:
        ss << "RANGE (" << part_expr << ")";
        break;
      case dd::Table::PT_RANGE_COLUMNS:
        ss << "RANGE COLUMNS(" << part_expr << ")";
        break;
      case dd::Table::PT_LIST:
        ss << "LIST (" << part_expr << ")";
        break;
      case dd::Table::PT_LIST_COLUMNS:
        ss << "LIST COLUMNS(" << part_expr << ")";
        break;
      default:
        break;
    }

    bool has_subparts = tbl.subpartition_type() != dd::Table::ST_NONE;
    if (has_subparts) {
      const auto &subpart_expr = tbl.subpartition_expression_utf8();
      switch (tbl.subpartition_type()) {
        case dd::Table::ST_HASH:
          ss << "\nSUBPARTITION BY HASH (" << subpart_expr << ")";
          break;
        case dd::Table::ST_LINEAR_HASH:
          ss << "\nSUBPARTITION BY LINEAR HASH (" << subpart_expr << ")";
          break;
        case dd::Table::ST_KEY_55:
        case dd::Table::ST_KEY_51:
          ss << "\nSUBPARTITION BY KEY (" << subpart_expr << ")";
          break;
        case dd::Table::ST_LINEAR_KEY_51:
        case dd::Table::ST_LINEAR_KEY_55:
          ss << "\nSUBPARTITION BY LINEAR KEY (" << subpart_expr << ")";
          break;
        default:
          break;
      }

      for (const auto part : *tbl.partitions()) {
        auto num_sub = part->subpartitions()->size();
        if (num_sub > 0) {
          ss << "\nSUBPARTITIONS " << num_sub;
          break;
        }
      }
    }

    bool need_part_defs =
        (tbl.partition_type() == dd::Table::PT_RANGE ||
         tbl.partition_type() == dd::Table::PT_RANGE_COLUMNS ||
         tbl.partition_type() == dd::Table::PT_LIST ||
         tbl.partition_type() == dd::Table::PT_LIST_COLUMNS);

    if (need_part_defs) {
      ss << "\n(";
      bool first_part = true;
      for (const auto part : *tbl.partitions()) {
        if (part->parent_partition_id() != dd::INVALID_OBJECT_ID) continue;
        if (!first_part) ss << ",\n ";
        first_part = false;
        ss << "PARTITION " << quote_identifier(part->name());

        if (tbl.partition_type() == dd::Table::PT_RANGE ||
            tbl.partition_type() == dd::Table::PT_RANGE_COLUMNS) {
          ss << " VALUES LESS THAN (";
          bool first_val = true;
          for (const auto pv : part->values()) {
            if (!first_val) ss << ",";
            first_val = false;
            if (pv->max_value())
              ss << "MAXVALUE";
            else if (pv->is_value_null())
              ss << "NULL";
            else
              ss << pv->value_utf8();
          }
          ss << ")";
        } else if (tbl.partition_type() == dd::Table::PT_LIST ||
                   tbl.partition_type() == dd::Table::PT_LIST_COLUMNS) {
          ss << " VALUES IN (";
          bool first_val = true;
          for (const auto pv : part->values()) {
            if (!first_val) ss << ",";
            first_val = false;
            if (pv->is_value_null())
              ss << "NULL";
            else
              ss << pv->value_utf8();
          }
          ss << ")";
        }

        ss << " ENGINE = " << part->engine();
      }
      ss << ")";
    } else {
      auto num_parts = tbl.partitions()->size();
      if (num_parts > 0) ss << "\nPARTITIONS " << num_parts;
    }
    ss << " */";
  }

  ss << ";\n";

  if (xtrabackup_export) {
    std::string path = std::string(xtrabackup_real_target_dir) + "/" +
                       std::string(schema_name.c_str()) + "/" +
                       std::string(tbl.name().c_str()) + ".sql";
    std::ofstream sql_file(path);
    if (sql_file.is_open()) {
      sql_file << ss.str();
      sql_file.close();
      DBUG_LOG("xb_schema", "wrote " << path);
    } else {
      xb::error() << "cannot write schema file " << path;
    }
  }
}

/** Load all InnoDB tables from space_id. There could be multiple tables
in a tablespace (general tablespace like mysql.ibd or a partition IBD (p0
contains all tables SDI). This function uses SDI to deserialize to dd::Table and
then convert to InnoDB table object dict_table_t*.
Whether to convert dd::Table object to dict_table_t or not is decided by
callback (load_table_cb)

@param[in] space_id      InnoDB tablespace_id
@param[in] table_id      InnoDB table id. If this is zero, we load *all* tables
                         found in space_id
@param[in] thd           Server thread context (used for DD APIs)
@param[in] trx           InnoDB trx object (for using SDI APIs)
@param[in] load_table_cb The callback function that processes dd::Table. A
                         callback can convert dd::Table to dict_table_t or
                         send the dd::Table to caller without conversion
@return DB_SUCCESS on success, other DB_* codes on errors */
static dberr_t dict_load_tables_from_space_id_low(
    space_id_t space_id, table_id_t table_id, THD *thd, trx_t *trx,
    std::function<dberr_t(dd_Table_Ptr dd_table, dd::String_type &schema_name)>
        load_table_cb) {
  sdi_vector_t sdi_vector;
  ib_sdi_vector_t ib_vector;
  ib_vector.sdi_vector = &sdi_vector;
  uint64 sdi_id = 0;

  if (!fsp_has_sdi(space_id)) {
    return DB_SUCCESS;
  }

  fil_space_t *space = fil_space_get(space_id);
  ut_ad(space != nullptr);
  if (space == nullptr) {
    return DB_TABLESPACE_NOT_FOUND;
  }

  uint32_t compressed_buf_len = 8 * 1024 * 1024;
  uint32_t uncompressed_buf_len = 16 * 1024 * 1024;
  auto compressed_sdi = ut_make_unique_ptr_zalloc_nokey(compressed_buf_len);
  auto sdi = ut_make_unique_ptr_zalloc_nokey(uncompressed_buf_len);

  ib_err_t err = ib_sdi_get_keys(space_id, &ib_vector, trx);

  if (err != DB_SUCCESS) {
    return err;
  }

  /* Before 8.0.24 if the table is used in EXCHANGE PARTITION or IMPORT. Even
  after upgrade to the latest version 8.0.25 (which fixed the duplicate SDI
  issue), such tables continue to contain duplicate SDI. PXB will scan the DD
  table "mysql.tables" to determine the correct SDI */
  if (ib_vector.sdi_vector->m_vec.size() > 2 &&
      strcmp(space->name, "mysql") != 0 &&
      fsp_is_file_per_table(space_id, space->flags)) {
    std::string table_name;
    err = get_sdi_id_from_dd(space->name, &sdi_id, table_name, thd);
    xb::info() << "duplicate SDI found for tablespace " << space->name
               << ". To remove duplicate SDI, "
                  "please execute OPTIMIZE TABLE on "
               << table_name.c_str();
    if (err != DB_SUCCESS) {
      return err;
    }
  }

  for (sdi_container::iterator it = ib_vector.sdi_vector->m_vec.begin();
       it != ib_vector.sdi_vector->m_vec.end(); it++) {
    ib_sdi_key_t ib_key;
    ib_key.sdi_key = &(*it);

    uint32_t compressed_sdi_len = compressed_buf_len;
    uint32_t uncompressed_sdi_len = uncompressed_buf_len;

    if (ib_key.sdi_key->type != 1 /* dd::Sdi_type::TABLE */) {
      continue;
    }

    /* In case of duplicate SDIs, sdi_id is the latest id according to DD, so we
    skip other dd::Table SDIs in the IBD file */
    if (sdi_id != 0 && ib_key.sdi_key->id != sdi_id) {
      continue;
    }

    while (true) {
      err = ib_sdi_get(space_id, &ib_key, compressed_sdi.get(),
                       &compressed_sdi_len, &uncompressed_sdi_len, trx);
      if (err == DB_OUT_OF_MEMORY) {
        compressed_buf_len = compressed_sdi_len;
        compressed_sdi = ut_make_unique_ptr_zalloc_nokey(compressed_buf_len);
        continue;
      }
      break;
    }

    if (err != DB_SUCCESS) {
      return err;
    }

    if (uncompressed_buf_len < uncompressed_sdi_len) {
      uncompressed_buf_len = uncompressed_sdi_len;

      sdi = ut_make_unique_ptr_zalloc_nokey(uncompressed_buf_len);
    }

    Sdi_Decompressor decompressor(sdi.get(), uncompressed_sdi_len,
                                  compressed_sdi.get(), compressed_sdi_len);
    decompressor.decompress();

    dd_Table_Ptr dd_table{dd::create_object<dd::Table>()};
    dd::String_type schema_name;

    bool res = dd::deserialize(
        thd, dd::Sdi_type((const char *)sdi.get(), uncompressed_sdi_len),
        dd_table.get(), &schema_name);

    if (res) {
      return DB_ERROR;
    }

    show_create_table(space_id, dd_table.get(), schema_name);

    bool is_part = dd_table_is_partitioned(*dd_table.get());

    if (is_part) {
      auto end = dd_table->leaf_partitions()->end();

      if (table_id != 0) {
        auto i =
            std::search_n(dd_table->leaf_partitions()->begin(), end, 1,
                          table_id, [](const dd::Partition *p, table_id_t id) {
                            return (p->se_private_id() == id);
                          });
        if (i == end) {
          continue;
        }
      }
    } else {
      uint64 se_private_id = dd_table.get()->se_private_id();
      if (table_id != 0 && table_id != se_private_id) continue;
    }

    // Callback.
    err = load_table_cb(std::move(dd_table), schema_name);
    // Do not use dd_table from here. The ownership has been transferred to
    // callback
    ut_ad(dd_table == nullptr);

    if (err != DB_SUCCESS) {
      return err;
    }
  }

  return (DB_SUCCESS);
}

/** This function uses dict_load_tables_from_space_id_low() with a callback
that loads all tables from dd::table into a vector
@param[in] space_id      InnoDB tablespace_id
@param[in] table_id      InnoDB table id. If this is zero, we load *all* tables
                         found in space_id
@param[in] thd           Server thread context (used for DD APIs)
@param[in] trx           InnoDB trx object (for using SDI APIs)
@return tuple <a,b>
a - DB_SUCCESS on success, other DB_ on errors
b - std::vector<dict_table_t*>, empty on errors */
xb_dict_tuple dict_load_tables_from_space_id(space_id_t space_id,
                                             table_id_t table_id, THD *thd,
                                             trx_t *trx) {
  std::vector<dict_table_t *> tables_vec;

  auto load_table_func = [&](dd_Table_Ptr dd_table,
                             dd::String_type &schema_name) -> dberr_t {
    fil_space_t *space = fil_space_get(space_id);

    using Client = dd::cache::Dictionary_client;
    using Releaser = dd::cache::Dictionary_client::Auto_releaser;

    Client *dc = dd::get_dd_client(thd);
    Releaser releaser{dc};

    ut_a(space != nullptr);

    bool implicit = fsp_is_file_per_table(space_id, space->flags);
    if (space_id == dict_sys_t::s_dict_space_id &&
        schema_name != MYSQL_SCHEMA_NAME.str) {
      schema_name = MYSQL_SCHEMA_NAME.str;
    }

    /* All tables in mysql.ibd should belong to 'mysql' schema. But
    during upgrade, server leaves the DD tables in a temporary schema
    'dd_upgrade_80XX". PXB reads DD tables using 'mysql' schema name.
    For example, 'mysql/tables'. Fix the schema name to 'mysql' */
    if (space_id == dict_sys_t::s_dict_space_id &&
        schema_name != MYSQL_SCHEMA_NAME.str) {
      schema_name = MYSQL_SCHEMA_NAME.str;
    }

    int ret;
    std::vector<dict_table_t *> tables;
    std::tie(ret, tables) = dd_table_load_on_dd_obj(
        dc, space_id, *dd_table.get(), table_id, thd, &schema_name, implicit);
    if (ret != 0) {
      return (DB_ERROR);
    } else {
      tables_vec.insert(std::end(tables_vec), std::begin(tables),
                        std::end(tables));
      return (DB_SUCCESS);
    }
  };

  dberr_t err = dict_load_tables_from_space_id_low(space_id, table_id, thd, trx,
                                                   load_table_func);

  return {err, tables_vec};
}

/** This function uses dict_load_tables_from_space_id_low() with a callback
that returns the dd::Table object to caller. We DONT convert dd::Table to
dict_table_t here
@param[in] space_id      InnoDB tablespace_id
@param[in] table_id      InnoDB table id. If this is zero, we load *all* tables
                         found in space_id
@return dd::Table object on success, else nullptr */
dd_Table_Ptr get_dd_Table(space_id_t space_id, table_id_t table_id) {
  dd_Table_Ptr tbl = dd_Table_Ptr{nullptr};

  // This callback doesn't convert the dd::Table object to InnoDB table
  // dict_table_t. This function returns dd::Table object to caller.
  auto load_table_func_cb = [&](dd_Table_Ptr dd_table,
                                dd::String_type &schema_name) -> dberr_t {
    tbl = std::move(dd_table);
    return (DB_SUCCESS);
  };

  THD *thd = current_thd;
  ut_a(thd != nullptr);

  ib_trx_t trx = ib_trx_begin(IB_TRX_READ_COMMITTED, false, false, thd);
#ifdef UNIV_DEBUG
  dberr_t err =
#endif
      dict_load_tables_from_space_id_low(space_id, table_id, thd, trx,
                                         load_table_func_cb);
  ib_trx_commit(trx);
  ib_trx_release(trx);

#ifdef UNIV_DEBUG
  if (err == DB_SUCCESS) {
    ut_ad(tbl != nullptr);
  } else {
    ut_ad(tbl == nullptr);
  }
#endif

  return (tbl);
}

/** @return all tables (dict_table_t*) from a tablespace
@param[in] space_id InnoDB tablespace id */
xb_dict_tuple dict_load_from_spaces_sdi(space_id_t space_id) {
  THD *thd = current_thd;
  ut_a(thd != nullptr);

  ib_trx_t trx = ib_trx_begin(IB_TRX_READ_COMMITTED, false, false, thd);

  /* Load mysql tablespace to open mysql/tables and mysql/schemata which is
  need to find the right key for tablespace in case of duplicate sdi */
  auto ret = dict_load_tables_from_space_id(space_id, 0, thd, trx);

  ib_trx_commit(trx);
  ib_trx_release(trx);

  return (ret);
}

/** Process one mysql.index_partitions record and load entries into the
following maps table_id_space_map, space_part_map, part_id_spaces_map
These maps are later used by PXB to load a specific table_id
@param[in]      heap            Temp memory heap
@param[in,out]  rec             mysql.index_partitions record
@param[in]      dd_indexes      dict_table_t obj of mysql.index_partitions
@retval true if index record is processed */
static bool process_dd_index_partitions_rec(mem_heap_t *heap, const rec_t *rec,
                                            dict_table_t *dd_indexes) {
  ulint len;
  const byte *field;
  uint32_t space_id;
  uint64_t table_id;

  ut_ad(!rec_get_deleted_flag(rec, dict_table_is_comp(dd_indexes)));

  ulint *offsets = rec_get_offsets(rec, dd_indexes->first_index(), nullptr,
                                   ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

  field = (const byte *)rec_get_nth_field(
      nullptr, rec, offsets, dd_indexes->field_number("index_id"), &len);

  uint64_t part_index_id = mach_read_from_8(field);
  ut_ad(len == 8);

  /* Get the se_private_data field. */
  field = (const byte *)rec_get_nth_field(
      nullptr, rec, offsets,
      dd_indexes->field_number("se_private_data") + DD_FIELD_OFFSET, &len);

  if (len == 0 || len == UNIV_SQL_NULL) {
    return false;
  }

  /* Get index id. */
  dd::String_type prop((char *)field);
  dd::Properties *p = dd::Properties::parse_properties(prop);

  if (!p || !p->exists(dd_index_key_strings[DD_TABLE_ID]) ||
      !p->exists(dd_index_key_strings[DD_INDEX_SPACE_ID])) {
    if (p) {
      delete p;
    }
    return false;
  }

  if (p->get(dd_index_key_strings[DD_TABLE_ID], &table_id)) {
    delete p;
    return false;
  }

  /* Get the tablespace id. */
  if (p->get(dd_index_key_strings[DD_INDEX_SPACE_ID], &space_id)) {
    delete p;
    return false;
  }

  if (table_id_space_map.find(table_id) == table_id_space_map.end()) {
    DBUG_LOG("xb_dd", "From mysql.index_partitions: Inserting into "
                          << "table_id_space_map: <" << table_id << ","
                          << space_id << ">");
    table_id_space_map.insert(std::make_pair(table_id, space_id));

    if (space_part_map.find(space_id) == space_part_map.end()) {
      DBUG_LOG("xb_dd", "From mysql.index_partitions: Inserting into "
                            << "space_part_map: <" << space_id << ","
                            << part_index_id << ">");
      space_part_map.insert(std::make_pair(space_id, part_index_id));
    }

    DBUG_LOG("xb_dd", "From mysql.index_partitions: Inserting into "
                          << "part_id_spaces_map: <" << part_index_id << ","
                          << space_id << ">");
    DBUG_LOG("xb_dd", "-----------------------------------------");

    part_id_spaces_map.insert(std::make_pair(part_index_id, space_id));
  }
  delete p;
  return true;
}

/** Process one mysql.indexes record and load entries into the
following maps table_id_space_map
These maps are later used by PXB to load a specific table_id
@param[in]      heap            Temp memory heap
@param[in,out]  rec             mysql.indexes record
@param[in]      dd_indexes      dict_table_t obj of mysql.indexes
@retval true if index record is processed */
static bool process_dd_indexes_rec(mem_heap_t *heap, const rec_t *rec,
                                   dict_table_t *dd_indexes) {
  ulint len;
  const byte *field;
  uint32_t space_id;
  uint64_t table_id;

  ut_ad(!rec_get_deleted_flag(rec, dict_table_is_comp(dd_indexes)));

  ulint *offsets = rec_get_offsets(rec, dd_indexes->first_index(), nullptr,
                                   ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
  field = rec_get_nth_field(
      nullptr, rec, offsets,
      dd_indexes->field_number("engine") + DD_FIELD_OFFSET, &len);

  /* If "engine" field is not "innodb", return. */
  if (strncmp((const char *)field, "InnoDB", 6) != 0) {
    return false;
  }

  /* Get the se_private_data field. */
  field = (const byte *)rec_get_nth_field(
      nullptr, rec, offsets,
      dd_indexes->field_number("se_private_data") + DD_FIELD_OFFSET, &len);

  if (len == 0 || len == UNIV_SQL_NULL) {
    return false;
  }

  /* Get index id. */
  dd::String_type prop((char *)field);
  dd::Properties *p = dd::Properties::parse_properties(prop);

  if (!p || !p->exists(dd_index_key_strings[DD_TABLE_ID]) ||
      !p->exists(dd_index_key_strings[DD_INDEX_SPACE_ID])) {
    if (p) {
      delete p;
    }
    return false;
  }

  if (p->get(dd_index_key_strings[DD_TABLE_ID], &table_id)) {
    delete p;
    return false;
  }

  /* Get the tablespace id. */
  if (p->get(dd_index_key_strings[DD_INDEX_SPACE_ID], &space_id)) {
    delete p;
    return false;
  }

  if (table_id_space_map.find(table_id) == table_id_space_map.end()) {
    DBUG_LOG("xb_dd", "From mysql.indexes: Inserting into table_id_space_map: <"
                          << table_id << "," << space_id << ">";);
    table_id_space_map.insert(std::make_pair(table_id, space_id));
  }

  delete p;
  return true;
}

/** Scan mysql.indexes and build a <table_id,space_id> map
@param[in] thd Server thread context
@return DB_SUCCESS on success, other DB_* on errors */
static dberr_t scan_mysql_indexes(THD *thd) {
  dict_table_t *dd_indexes = nullptr;
  btr_pcur_t pcur;
  const rec_t *rec = nullptr;
  mtr_t mtr;
  MDL_ticket *mdl = nullptr;
  mem_heap_t *heap = mem_heap_create(1000, UT_LOCATION_HERE);

  mtr_start(&mtr);

  rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, dd_indexes_name.c_str(),
                            &dd_indexes);

  while (rec != nullptr) {
    process_dd_indexes_rec(heap, rec, dd_indexes);

    mtr_commit(&mtr);

    mem_heap_empty(heap);

    mtr_start(&mtr);
    rec = (rec_t *)dd_getnext_system_rec(&pcur, &mtr);
  }

  mtr_commit(&mtr);
  dd_table_close(dd_indexes, thd, &mdl, true);
  mem_heap_free(heap);

  return (DB_SUCCESS);
}

/** Scan mysql.index_partitions and build following maps table_id_space_map,
space_part_map, part_id_spaces_map
@param[in] thd Server thread context
@return DB_SUCCESS on success, other DB_* on errors */
static dberr_t scan_mysql_index_partitions(THD *thd) {
  dict_table_t *dd_indexes = nullptr;
  btr_pcur_t pcur;
  const rec_t *rec = nullptr;
  mtr_t mtr;
  MDL_ticket *mdl = nullptr;
  mem_heap_t *heap = mem_heap_create(1000, UT_LOCATION_HERE);

  mtr_start(&mtr);

  rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, dd_index_partitions.c_str(),
                            &dd_indexes);

  while (rec != nullptr) {
    process_dd_index_partitions_rec(heap, rec, dd_indexes);

    mtr_commit(&mtr);

    mem_heap_empty(heap);

    mtr_start(&mtr);
    rec = (rec_t *)dd_getnext_system_rec(&pcur, &mtr);
  }

  mtr_commit(&mtr);
  dd_table_close(dd_indexes, thd, &mdl, true);
  mem_heap_free(heap);

  return (DB_SUCCESS);
}

/** Build dictionary required for prepare phase. Currently used
for rollback of transactions, export of tables (.cfg file creation)
and --stats features
@param[in] thd Server thread context
@return DB_SUCCESS on success or other DB_* codes on errors */
static dberr_t build_dictionary(THD *thd) {
  mutex_enter(&dict_sys->mutex);
  scan_mysql_indexes(thd);
  scan_mysql_index_partitions(thd);
  mutex_exit(&dict_sys->mutex);
  return (DB_SUCCESS);
}

/** Load all tables from mysql.ibd. This includes dictionary tables, system
tables
@return tuple <a,b>
a - DB_SUCCESS on success, other DB_ on errors
b - std::vector<dict_table_t*>, empty on errors */
xb_dict_tuple dict_load_from_mysql_ibd() {
  auto begin = std::chrono::high_resolution_clock::now();
  THD *thd = current_thd;
  ut_ad(thd != nullptr);
  auto result = dict_load_from_spaces_sdi(dict_sys_t::s_dict_space_id);
  dberr_t err = std::get<0>(result);
  if (err == DB_SUCCESS) {
    err = build_dictionary(thd);
  }
  auto end = std::chrono::high_resolution_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
  xb::info() << "Time taken to build dictionary: " << elapsed.count() * 1e-9
             << " seconds";

  return result;
}

/** Clear all the maps created to handle dictionary during prepare */
void clear_dd_cache_maps() {
  table_id_space_map.clear();
  space_part_map.clear();
  part_id_spaces_map.clear();
  sdi_id_map.clear();
  dd_schema_map.clear();
}

}  // namespace prepare

}  // namespace xb
