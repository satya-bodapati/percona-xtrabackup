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
#include "xbcloud/auth/credential_provider.h"
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

  // CredentialProvider consulted by sign(): BEARER-mode providers
  // (Managed Identity, AAD Service Principal) attach an
  // Authorization: Bearer <token> header and skip Shared Key
  // signing.  HMAC_SHARED_KEY-mode providers (the existing
  // SharedKeyProvider) let Azure_signer sign normally.  When
  // credential_provider_ is null, signing goes through Azure_signer
  // unchanged for backwards compatibility with call paths that
  // don't set a provider.
  auth::CredentialProvider *credential_provider_{nullptr};

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

  // Register a CredentialProvider that sign() consults before each
  // request.  Non-owning; caller keeps ownership (typically
  // Azure_object_store's credential_provider_ unique_ptr).
  void set_credential_provider(auth::CredentialProvider *p) {
    credential_provider_ = p;
  }

  // Sign wrapper: consults credential_provider_ (if any) and either
  // attaches Bearer auth (BEARER wire_mode) or delegates to the
  // Shared Key signer (default).  Every request path calls this
  // instead of signer->sign_request() directly.
  void sign(const std::string &container, const std::string &blob,
            Http_request &req, time_t t) {
    if (credential_provider_ != nullptr &&
        credential_provider_->wire_mode() == auth::WireMode::BEARER) {
      // Azure REST supports Authorization: Bearer <token> as an
      // alternative to Shared Key on the same endpoints, provided
      // the request also carries x-ms-version and x-ms-date.  The
      // Shared Key signer already adds those, so we invoke it but
      // then overwrite Authorization with the bearer header.
      signer->sign_request(container, blob, req, t);
      req.remove_header("Authorization");
      req.add_header("Authorization",
                     "Bearer " + credential_provider_->get_bearer());
    } else {
      signer->sign_request(container, blob, req, t);
    }
  }

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
};

class Azure_object_store : public Object_store {
 private:
  Azure_client azure_client;
  Http_request::headers_t extra_http_headers;
  // Owned CredentialProvider.  Set by xbcloud.cc at construction time
  // (SharedKeyProvider today; ManagedIdentityProvider will follow).
  // Azure_client keeps signing directly against its own copy of the
  // storage_account/access_key strings for now — Phase 5 wires the
  // Bearer path through the provider once ManagedIdentityProvider
  // lands, and Shared Key signing continues untouched.
  std::unique_ptr<auth::CredentialProvider> credential_provider_;

 public:
  void set_credential_provider(
      std::unique_ptr<auth::CredentialProvider> provider) {
    credential_provider_ = std::move(provider);
    azure_client.set_credential_provider(credential_provider_.get());
  }
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
    return azure_client.list_objects_with_prefix(container, directory + "/",
                                                 objects);
  }
  virtual bool upload_object(const std::string &container,
                             const std::string &object,
                             const Http_buffer &contents) override {
    return azure_client.upload_object(container, object, contents);
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
