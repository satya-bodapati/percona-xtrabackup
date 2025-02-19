#include "ds_checksum_sha256.h"
#include <mysql/service_mysql_alloc.h>
#include <mysql_version.h>
#include <openssl/evp.h>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include "common.h"
#include "datasink.h"
#include "manifest_writer.h"

/* Global context struct for all file operations */
typedef struct {
  const EVP_MD *hash_algorithm;             // Default algorithm (configurable)
  std::atomic<size_t> checksum_operations;  // Total checksum operations
} ds_checksum_ctxt_t;

/* Per-file structure for checksum datasink */
typedef struct ds_checksum_file {
  ds_file_t *dest_file;  // Underlying datasink (next in the chain)
  ds_checksum_ctxt_t *checksum_ctxt;
  EVP_MD_CTX *sha256_ctx;  // Per-file SHA-256 context
  size_t length;
  FileProperties *prop;
  std::string input_file_name;
} ds_checksum_file_t;

static ds_ctxt_t *ds_checksum_sha256_init(const char *root) {
  ds_checksum_ctxt_t *checksum_ctxt = new ds_checksum_ctxt_t;
  checksum_ctxt->hash_algorithm = EVP_sha256();
  checksum_ctxt->checksum_operations = 0;
  ds_ctxt_t *ctxt = new ds_ctxt_t;
  ctxt->ptr = checksum_ctxt;
  ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));

  return ctxt;
}
static ds_file_t *ds_checksum_sha256_open(ds_ctxt_t *ctxt, const char *filename,
                                          MY_STAT *mystat,
                                          FileProperties *prop) {
  ds_ctxt_t *dest_ctxt = ctxt->pipe_ctxt;
  ds_checksum_ctxt_t *checksum_ctxt = (ds_checksum_ctxt_t *)ctxt->ptr;

  // We dont intentionally pass the callback context below datasinks. nullptr is
  // passed instead.
  ds_file_t *dest_file = ds_open(dest_ctxt, filename, mystat, prop);
  if (!dest_file) {
    return nullptr;
  }

  ds_checksum_file_t *chk_file = new ds_checksum_file_t;
  chk_file->sha256_ctx = EVP_MD_CTX_new();  // Allocate EVP context
  if (!chk_file->sha256_ctx) {
    delete chk_file;
    return nullptr;
  }

  if (!EVP_DigestInit_ex(chk_file->sha256_ctx, checksum_ctxt->hash_algorithm,
                         nullptr)) {
    EVP_MD_CTX_free(chk_file->sha256_ctx);
    delete chk_file;
    return nullptr;
  }
  chk_file->dest_file = dest_file;
  chk_file->checksum_ctxt = checksum_ctxt;
  chk_file->length = 0;
  chk_file->prop = prop;
  chk_file->input_file_name = filename;

  ds_file_t *file = new ds_file_t;
  file->ptr = chk_file;
  file->path = dest_file->path;
  return file;
}

static int ds_checksum_sha256_write(ds_file_t *file, const void *buf,
                                    size_t size) {
  ds_checksum_file_t *chk_file =
      reinterpret_cast<ds_checksum_file_t *>(file->ptr);
  ds_checksum_ctxt_t *checksum_ctxt = chk_file->checksum_ctxt;

  EVP_DigestUpdate(chk_file->sha256_ctx, buf, size);
  // Increment global checksum operation counter
  checksum_ctxt->checksum_operations++;
  chk_file->length += size;
  return ds_write(chk_file->dest_file, buf, size);
}

static int ds_checksum_sha256_close(ds_file_t *file) {
  ds_checksum_file_t *chk_file =
      reinterpret_cast<ds_checksum_file_t *>(file->ptr);
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;

  EVP_DigestFinal_ex(chk_file->sha256_ctx, hash, &hash_len);
  EVP_MD_CTX_free(chk_file->sha256_ctx);  // Free context

  if (chk_file->prop != nullptr) {
    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
      oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    chk_file->prop->emplace_back("SHA256Checksum", oss.str());
    chk_file->prop->emplace_back("Total_len_bytes", chk_file->length);
    addFileEntryifEnabled(chk_file->input_file_name, *chk_file->prop);
  }

  int ret = ds_close(chk_file->dest_file);
  delete file;
  delete chk_file;

  return ret;
}

static void ds_checksum_sha256_deinit(ds_ctxt_t *ctxt) {
  if (!ctxt || !ctxt->ptr) return;

  ds_checksum_ctxt_t *checksum_ctxt = (ds_checksum_ctxt_t *)ctxt->ptr;
  std::cerr << "Total checksum operations performed: "
            << checksum_ctxt->checksum_operations << std::endl;
  delete checksum_ctxt;

  my_free(ctxt->root);
  delete ctxt;
}

/* Define the datasink */
datasink_t datasink_checksum_sha256 = {
    &ds_checksum_sha256_init,  &ds_checksum_sha256_open,
    &ds_checksum_sha256_write, nullptr,
    &ds_checksum_sha256_close, &ds_checksum_sha256_deinit};
