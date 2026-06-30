/******************************************************
Copyright (c) 2021 Percona LLC and/or its affiliates.

AZURE client implementation.

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

#ifndef __XBCLOUD_AZURE_H__
#define __XBCLOUD_AZURE_H__

#include <iostream>
#include "object_store.h"
#include "xbcloud/http.h"
#include "xbcloud/util.h"

#include <time.h>
namespace xbcloud {

class Azure_response {
 private:
  bool error_{false};
  std::string error_message_;
  std::string error_code_;

 public:
  Azure_response() {}
  bool parse_http_response(Http_response &http_response);
  bool error() const { return error_; }
  const std::string &error_message() { return error_message_; }
  const std::string &error_code() { return error_code_; }
};

class Azure_signer {
 private:
  std::string storage_account;
  std::string access_key;
  bool development_storage;
  std::string storage_class;

  static std::string azure_date_format(time_t t);

  std::string build_string_to_sign(Http_request &request,
                                   const std::string &container,
                                   const std::string &blob);

 public:
  Azure_signer(const std::string &storage_account,
               const std::string &access_key, bool development_storage,
               const std::string &storage_class = std::string())
      : storage_account(storage_account),
        access_key(access_key),
        development_storage(development_storage),
        storage_class(storage_class) {}
  void sign_request(const std::string &container, const std::string &blob,
                    Http_request &req, time_t t);
  virtual ~Azure_signer() {}
};

class Azure_client {
 public:
  using async_upload_callback_t =
      std::function<void(bool, const Http_buffer &)>;
  using async_download_callback_t =
      std::function<void(bool, const Http_buffer &)>;
  using async_delete_callback_t = std::function<void(bool)>;

  std::unique_ptr<Azure_signer> signer;

 private:
  const Http_client *http_client;

  std::string endpoint;
  std::string host;
  std::string storage_account;
  std::string access_key;

  std::string session_token;

  Http_request::protocol_t protocol;
  ulong max_retries;
  ulong max_backoff;

  static void upload_callback(Azure_client *client, std::string container,
                              std::string name, Http_request *req,
                              Http_response *resp,
                              const Http_client *http_client, Event_handler *h,
                              Azure_client::async_upload_callback_t callback,
                              CURLcode rc, const Http_connection *conn,
                              ulong count);

  static void download_callback(
      Azure_client *client, std::string container, std::string name,
      Http_request *req, Http_response *resp, const Http_client *http_client,
      Event_handler *h, Azure_client::async_download_callback_t callback,
      CURLcode rc, const Http_connection *conn, ulong count);

  // Common helper function for listing objects - handles pagination and XML
  // parsing ProcessBlob is a callable that takes (rapidxml::xml_node<>* node)
  // and returns bool Returns false to stop processing, true to continue
  template <typename ProcessBlob>
  bool list_objects_common(const std::string &container,
                           const std::string &prefix,
                           ProcessBlob &&process_blob);

 public:
  Azure_client(const Http_client *client, const std::string &storage_account,
               const std::string &access_key, bool development_storage,
               const std::string &storage_class, const ulong max_retries,
               const ulong max_backoff);

  void set_endpoint(const std::string &ep, bool development_storage,
                    const std::string &storage_account);

  bool delete_object(const std::string &container, const std::string &name);

  bool create_container(const std::string &name);

  bool container_exists(const std::string &name, bool &exists);

  bool upload_object(const std::string &container, const std::string &name,
                     const Http_buffer &contents);

  Http_buffer download_object(const std::string &container,
                              const std::string &name, bool &success);

  bool async_upload_object(
      const std::string &container, const std::string &name,
      const Http_buffer &contents, Event_handler *h,
      async_upload_callback_t callback = {},
      const Http_request::headers_t &extra_http_headers = {});

  bool async_download_object(
      const std::string &container, const std::string &name, Event_handler *h,
      async_download_callback_t callback = {},
      const Http_request::headers_t &extra_http_headers = {});

  bool async_delete_object(const std::string &container,
                           const std::string &name, Event_handler *h,
                           const async_delete_callback_t callback);

  bool list_objects_with_prefix(const std::string &container,
                                const std::string &prefix,
                                std::vector<std::string> &objects);

  /**
   * List objects under a prefix and split them into files and directories.
   *
   * For HNS-enabled containers, directory entries are returned explicitly.
   *
   * @param container Container name.
   * @param prefix Prefix to list.
   * @param files Output list of file objects.
   * @param dirs Output list of directory objects.
   * @return true on success, false on error.
   */
  bool list_objects_files_and_dirs(const std::string &container,
                                   const std::string &prefix,
                                   std::vector<std::string> &files,
                                   std::vector<std::string> &dirs);

  ulong get_max_retries() { return max_retries; }

  ulong get_max_backoff() { return max_backoff; }

  void retry_error(Http_response *resp, bool *retry) {}

  /* Not used */
  std::string hostname(const std::string &not_used) const { return host; }

  /* Multipart upload (PXB-3671 prototype).
     Azure block blob uses Put Block + Put Block List instead of S3-style
     multipart. There is no explicit "init" call; the first Put Block opens
     the blob's uncommitted block list implicitly. init_multipart_upload
     here just mints a session marker. */
  bool init_multipart_upload(const std::string &container,
                             const std::string &name, std::string &upload_id);
  bool upload_part(const std::string &container, const std::string &name,
                   const std::string &upload_id, int part_number,
                   const Http_buffer &contents, std::string &block_id);
  /* PXB-3671: async variant via Event_handler. Submits Put Block then
     fires the callback with the block_id on completion. */
  using part_callback_t = std::function<void(bool ok, std::string block_id)>;
  bool async_upload_part(const std::string &container, const std::string &name,
                         const std::string &upload_id, int part_number,
                         const Http_buffer &contents, Event_handler *h,
                         part_callback_t callback);
  bool complete_multipart_upload(
      const std::string &container, const std::string &name,
      const std::string &upload_id,
      const std::vector<std::pair<int, std::string>> &parts);
  bool abort_multipart_upload(const std::string &container,
                              const std::string &name,
                              const std::string &upload_id);
};

class Azure_object_store : public Object_store {
 private:
  Azure_client azure_client;
  Http_request::headers_t extra_http_headers;

 public:
  Azure_object_store(const Http_client *client,
                     const std::string &storage_account,
                     const std::string &access_key, bool development_storage,
                     const std::string &storage_class, const ulong max_retries,
                     const ulong max_backoff,
                     const std::string &endpoint = std::string())
      : azure_client{
            client,        storage_account, access_key, development_storage,
            storage_class, max_retries,     max_backoff} {
    if (!endpoint.empty())
      azure_client.set_endpoint(endpoint, development_storage, storage_account);
  }
  void set_extra_http_headers(const Http_request::headers_t &headers) {
    extra_http_headers = headers;
  }
  virtual bool create_container(const std::string &name) override {
    return azure_client.create_container(name);
  }
  virtual bool container_exists(const std::string &name,
                                bool &exists) override {
    return azure_client.container_exists(name, exists);
  }
  virtual bool list_objects_in_directory(
      const std::string &container, const std::string &directory,
      std::vector<std::string> &objects) override {
    /* See S3_object_store::list_objects_in_directory for the empty-
    directory rationale.  Same applies to Azure: prefix="" must remain
    empty so the listing returns every blob in the container. */
    const std::string prefix =
        directory.empty() ? std::string{} : directory + "/";
    return azure_client.list_objects_with_prefix(container, prefix, objects);
  }
  virtual bool upload_object(const std::string &container,
                             const std::string &object,
                             const Http_buffer &contents) override {
    return azure_client.upload_object(container, object, contents);
  }

  virtual bool init_multipart_upload(const std::string &container,
                                     const std::string &object,
                                     std::string &upload_id) override {
    return azure_client.init_multipart_upload(container, object, upload_id);
  }
  virtual bool upload_part(const std::string &container,
                           const std::string &object,
                           const std::string &upload_id, int part_number,
                           const Http_buffer &contents,
                           std::string &part_id) override {
    return azure_client.upload_part(container, object, upload_id, part_number,
                                    contents, part_id);
  }
  virtual bool upload_part_async(
      const std::string &container, const std::string &object,
      const std::string &upload_id, int part_number,
      const Http_buffer &contents, Event_handler *h,
      std::function<void(bool, std::string)> callback) override {
    return azure_client.async_upload_part(container, object, upload_id,
                                          part_number, contents, h,
                                          std::move(callback));
  }
  virtual bool complete_multipart_upload(
      const std::string &container, const std::string &object,
      const std::string &upload_id,
      const std::vector<multipart_part_t> &parts) override {
    return azure_client.complete_multipart_upload(container, object, upload_id,
                                                  parts);
  }
  virtual bool abort_multipart_upload(const std::string &container,
                                      const std::string &object,
                                      const std::string &upload_id) override {
    return azure_client.abort_multipart_upload(container, object, upload_id);
  }
  virtual bool async_upload_object(
      const std::string &container, const std::string &object,
      const Http_buffer &contents, Event_handler *h,
      std::function<void(bool, const Http_buffer &contents)> f = {}) override {
    return azure_client.async_upload_object(
        container, object, contents, h,
        [f](bool success, const Http_buffer &contents) {
          if (f) f(success, contents);
        },
        extra_http_headers);
  }
  virtual bool async_download_object(
      const std::string &container, const std::string &object, Event_handler *h,
      const std::function<void(bool, const Http_buffer &contents)> f = {})
      override {
    return azure_client.async_download_object(
        container, object, h,
        [f](bool success, const Http_buffer &contents) {
          if (f) f(success, contents);
        },
        extra_http_headers);
  }
  virtual bool async_delete_object(const std::string &container,
                                   const std::string &object, Event_handler *h,
                                   std::function<void(bool)> f = {}) override {
    return azure_client.async_delete_object(container, object, h,
                                            [f](bool success) {
                                              if (f) f(success);
                                            });
  }
  virtual bool delete_object(const std::string &container,
                             const std::string &name) override {
    return azure_client.delete_object(container, name);
  }
  virtual Http_buffer download_object(const std::string &container,
                                      const std::string &name,
                                      bool &success) override {
    return azure_client.download_object(container, name, success);
  }
  virtual bool list_objects_files_and_dirs(
      const std::string &container, const std::string &directory,
      std::vector<std::string> &files,
      std::vector<std::string> &dirs) override {
    return azure_client.list_objects_files_and_dirs(container, directory + "/",
                                                    files, dirs);
  }
};
}  // namespace xbcloud

#endif
