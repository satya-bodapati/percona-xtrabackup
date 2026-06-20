/******************************************************
Copyright (c) 2019 Percona LLC and/or its affiliates.

Object Store interface.

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

#ifndef XBCLOUD_OBJECT_STORE
#define XBCLOUD_OBJECT_STORE

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "http.h"

namespace xbcloud {

class Event_handler;

/* Per-part identifier returned by upload_part and consumed by
   complete_multipart_upload. Part number is 1-based on S3/GCS. The opaque
   string is the ETag (S3/GCS), block id (Azure), or segment path (Swift). */
using multipart_part_t = std::pair<int, std::string>;

class Object_store {
 public:
  virtual bool create_container(const std::string &name) = 0;
  virtual bool container_exists(const std::string &name, bool &exists) = 0;
  virtual bool list_objects_in_directory(const std::string &container,
                                         const std::string &directory,
                                         std::vector<std::string> &objects) = 0;
  virtual bool upload_object(const std::string &container,
                             const std::string &object,
                             const Http_buffer &contents) = 0;
  virtual bool async_upload_object(
      const std::string &container, const std::string &object,
      const Http_buffer &contents, Event_handler *h,
      std::function<void(bool, const Http_buffer &contents)> f = {}) = 0;
  virtual bool async_download_object(
      const std::string &container, const std::string &object, Event_handler *h,
      std::function<void(bool, const Http_buffer &contents)> f = {}) = 0;
  virtual bool async_delete_object(const std::string &container,
                                   const std::string &object, Event_handler *h,
                                   std::function<void(bool)> f = {}) = 0;
  virtual bool delete_object(const std::string &container,
                             const std::string &name) = 0;
  virtual Http_buffer download_object(const std::string &container,
                                      const std::string &name,
                                      bool &success) = 0;
  /**
   * List objects under a directory prefix and split them into files and dirs.
   *
   * Default implementation treats all returned objects as files.
   *
   * @param container Container/bucket name.
   * @param directory Directory prefix to list.
   * @param files Output list of file objects.
   * @param dirs Output list of directory objects (may be empty).
   * @return true on success, false on error.
   */
  virtual bool list_objects_files_and_dirs(const std::string &container,
                                           const std::string &directory,
                                           std::vector<std::string> &files,
                                           std::vector<std::string> &dirs);

  /* Multipart upload interface (PXB-3671 prototype).

     Allows callers to upload a single logical object as several parts in
     sequence, producing one bucket object with the real file name (no
     ".NNNNN" chunk suffix). Backends that do not implement multipart return
     false from init_multipart_upload, and the caller is expected to fall
     back to the chunk-PUT path. */

  /* Open a new multipart upload session. On success, fills upload_id with a
     backend-opaque handle that must be passed to subsequent calls. */
  virtual bool init_multipart_upload(const std::string &container,
                                     const std::string &object,
                                     std::string &upload_id) {
    (void)container;
    (void)object;
    (void)upload_id;
    return false;
  }

  /* Upload one part of an open multipart session. part_number is 1-based.
     On success, fills part_id with the backend's part identifier (ETag for
     S3/GCS, block id for Azure, segment path for Swift). */
  virtual bool upload_part(const std::string &container,
                           const std::string &object,
                           const std::string &upload_id, int part_number,
                           const Http_buffer &contents, std::string &part_id) {
    (void)container;
    (void)object;
    (void)upload_id;
    (void)part_number;
    (void)contents;
    (void)part_id;
    return false;
  }

  /* Finalize the multipart upload. parts must be the ordered list of
     part_number/part_id pairs returned by upload_part. */
  virtual bool complete_multipart_upload(
      const std::string &container, const std::string &object,
      const std::string &upload_id,
      const std::vector<multipart_part_t> &parts) {
    (void)container;
    (void)object;
    (void)upload_id;
    (void)parts;
    return false;
  }

  /* Cancel an open multipart upload. Removes any parts already uploaded. */
  virtual bool abort_multipart_upload(const std::string &container,
                                      const std::string &object,
                                      const std::string &upload_id) {
    (void)container;
    (void)object;
    (void)upload_id;
    return false;
  }

  /* Async variant of upload_part. Submits the part through the given
     Event_handler and fires the callback with (ok, part_id) when the part
     either completes or fails. Default impl falls back to sync upload_part
     and invokes the callback synchronously, so backends can opt in to true
     async by overriding. */
  virtual bool upload_part_async(
      const std::string &container, const std::string &object,
      const std::string &upload_id, int part_number,
      const Http_buffer &contents, Event_handler *h,
      std::function<void(bool ok, std::string part_id)> callback) {
    (void)h;
    std::string part_id;
    bool ok = upload_part(container, object, upload_id, part_number, contents,
                          part_id);
    callback(ok, std::move(part_id));
    return true;
  }

  virtual ~Object_store() {}
};

}  // namespace xbcloud

#endif  // XBCLOUD_OBJECT_STORE
