/******************************************************
Copyright (c) 2019, 2021 Percona LLC and/or its affiliates.

HTTP client implementation using cURL.

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

#ifndef XBCLOUD_HTTP_H
#define XBCLOUD_HTTP_H

#include <curl/curl.h>
#include <ev.h>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <my_sys.h>
#include <mysql/service_mysql_alloc.h>

#include "xbcloud/hash.h"

namespace xbcloud {

using curl_easy_unique_ptr =
    std::unique_ptr<::CURL, decltype(&curl_easy_cleanup)>;
inline curl_easy_unique_ptr make_curl_easy() {
  return curl_easy_unique_ptr(curl_easy_init(), curl_easy_cleanup);
}

bool http_init();
void http_cleanup();

std::string uri_escape_string(const std::string &s);
std::string uri_escape_path(const std::string &path);

class Http_buffer {
 private:
  char *buf{nullptr};
  size_t buflen{0};
  size_t length{0};

  mutable std::vector<unsigned char> md5_;
  mutable std::vector<unsigned char> sha256_;

 public:
  using iterator = char *;
  using const_iterator = const char *;

  Http_buffer() {}
  Http_buffer(const Http_buffer &) = delete;
  Http_buffer(Http_buffer &&other) { *this = std::move(other); }
  Http_buffer &operator=(Http_buffer &&other) {
    buf = other.buf;
    buflen = other.buflen;
    length = other.length;
    md5_ = std::move(other.md5_);
    sha256_ = std::move(other.sha256_);
    other.buf = nullptr;
    other.length = other.buflen = 0;
    return *this;
  }
  ~Http_buffer() { my_free(buf); }

  void append(const char *b, size_t n) {
    reserve(size() + n);
    memcpy(buf + length, b, n);
    length += n;
    if (!md5_.empty()) md5_.clear();
    if (!sha256_.empty()) sha256_.clear();
  }
  void append(const std::string &s) { append(s.c_str(), s.size()); }
  void append(const char *s) { append(s, strlen(s)); }
  void append(const std::vector<char> &v) { append(&v[0], v.size()); }
  void append(const Http_buffer &b) { append(b.begin(), b.size()); }
  size_t size() const noexcept { return length; }
  char &operator[](size_t i) { return buf[i]; }
  const char &operator[](size_t i) const { return buf[i]; }
  iterator begin() noexcept { return buf; }
  const_iterator begin() const noexcept { return buf; }
  iterator end() noexcept { return buf + length; }
  const_iterator end() const noexcept { return buf + length; }
  size_t capacity() const noexcept { return buflen; }
  void reserve(size_t n) {
    if (buflen < n) {
      buf = static_cast<char *>(
          my_realloc(PSI_NOT_INSTRUMENTED, buf, n, MYF(MY_FAE)));
      buflen = n;
    }
  }
  void clear() noexcept {
    length = 0;
    md5_.clear();
    sha256_.clear();
  }
  void assign_buffer(char *buffer, size_t buffer_len, size_t len) {
    buf = buffer;
    buflen = buffer_len;
    length = len;
    md5_.clear();
    sha256_.clear();
  }
  std::vector<unsigned char> md5() const {
    if (md5_.empty()) {
      md5_ = xbcloud::md5(*this);
    }
    return md5_;
  }
  std::vector<unsigned char> sha256() const {
    if (sha256_.empty()) {
      sha256_ = xbcloud::sha256(*this);
    }
    return sha256_;
  }
};

class Http_request {
 public:
  enum method_t { GET, PUT, POST, DELETE, HEAD };
  enum protocol_t { HTTP, HTTPS };

  using header_t = std::pair<std::string, std::string>;
  using headers_t = std::map<std::string, std::string>;
  using param_t = std::pair<std::string, std::string>;
  using params_t = std::map<std::string, std::string>;

 private:
  method_t method_;
  protocol_t protocol_;
  std::string host_;
  std::string path_;
  headers_t headers_;
  params_t params_;
  Http_buffer payload_;

 public:
  Http_request(method_t method, protocol_t protocol, const std::string host,
               const std::string &path)
      : method_(method),
        protocol_(protocol),
        host_(host),
        path_(uri_escape_path(path)) {}
  void add_header(const std::string &name, const std::string &value) {
    headers_[name] = value;
  }
  void remove_header(const std::string &name) { headers_.erase(name); }
  void add_param(const std::string &name, const std::string &value) {
    params_[name] = value;
  }
  template <typename T>
  void append_payload(const T &payload) {
    payload_.append(payload);
  }
  void append_payload(const char *begin, size_t size) {
    payload_.append(begin, size);
  }
  std::string url() const {
    std::string qs = query_string();
    return (protocol_ == HTTP ? "http://" : "https://") + host_ + path_ +
           (!qs.empty() ? "?" + qs : "");
  }
  std::string path() const { return path_; }
  const headers_t &headers() const { return headers_; }
  bool has_header(const std::string &header_name) const {
    return headers_.count(header_name) > 0;
  }
  std::string header_value(const std::string &header_name) const {
    return headers_.at(header_name);
  }
  const params_t &params() const { return params_; }
  method_t method() const { return method_; }
  protocol_t protocol() const { return protocol_; }
  const Http_buffer &payload() const { return payload_; }
  std::string query_string() const;
};

class Http_response {
 private:
  long code{0};
  Http_buffer body_;
  std::map<std::string, std::string> headers_;

 public:
  Http_response() {}
  const Http_buffer &body() const { return body_; }
  Http_buffer move_body() { return std::move(body_); }
  const std::map<std::string, std::string> &headers() const { return headers_; }
  bool ok() const {
    if (code >= 200 && code < 300) {
      return true;
    }
    return false;
  }
  long http_code() const { return code; }
  void set_http_code(long http_code) { code = http_code; }
  static size_t header_appender(char *ptr, size_t size, size_t nmemb,
                                void *data);
  static size_t body_appender(char *ptr, size_t size, size_t nmemb,
                              void *data) {
    Http_response *response = reinterpret_cast<Http_response *>(data);
    response->body_.append(reinterpret_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
  }
  void reset_body() { body_.clear(); }
};

class Http_connection {
 public:
  struct upload_state_t {
    const char *data;
    size_t len;
  };
  using callback_t = std::function<void(CURLcode, Http_connection *)>;

 private:
  curl_easy_unique_ptr curl_;
  curl_slist *headers_;
  char error_[CURL_ERROR_SIZE];
  upload_state_t upload_state_;
  Http_response &response_;

  callback_t callback_;

 public:
  Http_connection(curl_easy_unique_ptr curl, const Http_request &request,
                  Http_response &response, callback_t callback = {})
      : curl_(std::move(curl)), response_(response), callback_(callback) {
    curl_easy_setopt(curl_.get(), CURLOPT_PRIVATE, this);
    curl_easy_setopt(curl_.get(), CURLOPT_ERRORBUFFER, error_);
  }

  ~Http_connection() { curl_slist_free_all(headers_); }

  CURL *curl_easy() const { return curl_.get(); }

  const char *error() const { return error_; }

  void set_headers(curl_slist *headers) { headers_ = headers; }

  void finalize(CURLcode rc) {
    long http_code;
    curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &http_code);
    response_.set_http_code(http_code);
    if (callback_) {
      callback_(rc, this);
    }
  }

  upload_state_t *upload_state() { return &upload_state_; }

  Http_response &response() const { return response_; }
};

/* PXB-3671: per-call timing aggregation for sync make_request.
   Catalogs total wall time and curl phase breakdowns by HTTP method so
   we can tell whether the multipart slowness vs legacy is per-call
   handshake (CONNECT + APPCONNECT > 0 on every call -> no connection
   reuse), DNS, signing, or the upstream itself. Dumped at xbcloud
   shutdown via dump_summary(). */
namespace http_timing {
struct Bucket {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> total_us{0};
  std::atomic<uint64_t> namelookup_us{0};
  std::atomic<uint64_t> connect_us{0};
  std::atomic<uint64_t> appconnect_us{0};
  std::atomic<uint64_t> pretransfer_us{0};
  std::atomic<uint64_t> starttransfer_us{0};
  std::atomic<uint64_t> calls_with_fresh_connect{0};
};
extern Bucket sync_get;
extern Bucket sync_post;
extern Bucket sync_put;
extern Bucket sync_delete;
extern Bucket sync_head;
extern std::atomic<bool> enabled;

void enable();
void dump_summary();
}  // namespace http_timing

class Event_handler {
 private:
  struct ev_loop *loop{nullptr};
  struct ev_timer timer_event;
  struct ev_async queue_event;
  struct ev_timer kickoff_event;
  /* Optional periodic rate-logger timer. Re-uses the existing libev
     loop -- no new thread. Fires every rate_log_interval seconds, reads
     the process-wide stats:: atomic counters, and logs throughput. */
  struct ev_timer rate_log_event;
  bool rate_log_enabled{false};
  double rate_log_interval{0.0};
  double rate_log_last_time{0.0};
  uint64_t rate_log_last_uploaded{0};
  uint64_t rate_log_last_appended{0};
  CURLM *curl_multi{nullptr};
  int running_handles{0};
  std::mutex queue_mutex;
  /* PXB-3748: producer blocks on this cv when the queue is full; the cv is
     notified by process_queue() whenever it pops items from the queue,
     replacing the previous 50 ms sleep_for spin-wait. */
  std::condition_variable queue_cv;
  size_t n_queued{0};
  size_t max_requests;
  bool final{false};
  bool loop_running{false};

  struct Curl_socket_info {
    curl_socket_t sockfd;
    CURL *curl_easy;
    int action;
    long timeout;
    struct ev_io ev;
    bool evset;
  };

  std::queue<Http_connection *> queue;

  static void mcode_or_die(CURLMcode code);

  void remove_socket(Curl_socket_info *socket_info);

  void set_socket(Curl_socket_info *socket_info, curl_socket_t sockfd,
                  CURL *curl_easy, int action);

  void add_socket(curl_socket_t sockfd, CURL *curl_easy, int action);

  static int multi_timer_callback(CURLM *multi, long timeout_ms, void *data);

  static int multi_socket_callback(CURL *curl_easy, curl_socket_t sockfd,
                                   int what, void *data, void *socket_data);

  void check_multi_info();

  static void ev_socket_callback(EV_P_ struct ev_io *io, int events);

  static void ev_timer_callback(EV_P_ struct ev_timer *timer, int events);

  static void ev_kickoff_callback(EV_P_ struct ev_timer *timer, int events);

  static void ev_queue_callback(EV_P_ ev_async *ev, int revents);

  static void ev_rate_log_callback(EV_P_ struct ev_timer *timer, int events);

  void main_loop();

  void process_queue();

 public:
  Event_handler(int max_requests) : max_requests(max_requests) {}

  ~Event_handler();

  bool init();

  std::thread run();

  /* Install a periodic rate-logger on the libev loop. Call after init()
     and before run(). interval_secs == 0 disables. Logs one line every
     interval_secs with bytes/s upload throughput and current in-flight
     counts. Runs on the libev thread (no new thread spawned). */
  void install_rate_logger(double interval_secs);

  void add_connection(Http_connection *conn, bool nowait = false);

  void stop();
};

class Http_client {
 public:
  using async_callback_t = std::function<void(CURLcode, Http_connection *)>;
 private:
  bool insecure{false};
  bool verbose{false};
  std::string cacert;
  /*
   * CURLcode::CURLE_OBSOLETE16 is used as backwards compatible error.
   * On newer versions of curl library it translates to CURLcode::CURLE_HTTP2.
   */
  std::vector<CURLcode> curl_retriable_errors{
      CURLcode::CURLE_GOT_NOTHING,       CURLcode::CURLE_OPERATION_TIMEDOUT,
      CURLcode::CURLE_RECV_ERROR,        CURLcode::CURLE_SEND_ERROR,
      CURLcode::CURLE_SEND_FAIL_REWIND,  CURLcode::CURLE_PARTIAL_FILE,
      CURLcode::CURLE_SSL_CONNECT_ERROR, CURLcode::CURLE_OBSOLETE16};
  std::vector<long> http_retriable_errors{503, 500, 504, 408};
  ulong timeout = 0;
  ulong max_retries = 10;
  ulong max_backoff = 300000;
  mutable curl_easy_unique_ptr curl{nullptr, curl_easy_cleanup};

  /* PXB-3671: shared connection pool across per-call easy handles.
     Without this, the thread-safety-driven per-call curl_easy pattern
     issues a fresh DNS lookup + TCP + TLS handshake on every sync
     request -- dominant cost over WAN.  CURLSH with CURL_LOCK_DATA_*
     pools DNS / SSL_SESSION / CONNECT entries across handles. Locking
     is provided by curl_share_mutex below.
     Initialized lazily in init_share(); destroyed in destructor. */
  mutable CURLSH *curl_share{nullptr};
  /* libcurl invokes our lock/unlock for each shared resource. We use a
     single mutex per resource type; the small array is indexed by the
     libcurl CURL_LOCK_DATA_* enum value. */
  mutable std::mutex curl_share_mutex[8];

  void init_share() const;

  static void async_result_callback(async_callback_t user_callback,
                                    Event_handler *h, CURLcode rc,
                                    Http_connection *conn);

  void setup_request(CURL *curl, const Http_request &request,
                     Http_response &response, curl_slist *&headers,
                     Http_connection::upload_state_t *upload_state) const;

  static int upload_callback(char *ptr, size_t size, size_t nmemb, void *data);

 public:
  Http_client() { init_share(); }
  virtual ~Http_client() {
    if (curl_share != nullptr) curl_share_cleanup(curl_share);
  }
  Http_client(const Http_client &) = delete;

  virtual bool make_request(const Http_request &request,
                            Http_response &response) const;

  /* Overload that captures the curl rc so callers (notably
     make_request_with_retry) can distinguish transient curl errors
     (CURLE_OPERATION_TIMEDOUT, CURLE_RECV_ERROR, ...) from permanent
     ones (CURLE_COULDNT_RESOLVE_HOST). */
  virtual bool make_request(const Http_request &request,
                            Http_response &response, CURLcode &out_rc) const;

  /**
   * Sync request with the same retriable-error + exponential-backoff
   * loop that make_async_request runs around the part-upload path.
   * Re-signs the request before each attempt so signature timestamps
   * stay fresh across the backoff window (SigV4 has a 15-minute
   * tolerance; long backoffs would otherwise exceed it).
   *
   * CLIENT must expose:
   *   - get_max_retries() / get_max_backoff()
   *   - hostname(container)
   *   - signer->sign_request(host, container, req, time_t)
   *   - retry_error(&resp, &flag) for body-level retry decisions
   *
   * This is the sync counterpart of Http_client::callback<>().
   * Covers init / complete / abort / list / delete / container ops
   * across S3, Azure, and Swift.
   */
  template <typename CLIENT>
  bool make_request_with_retry(CLIENT *client, const std::string &container,
                               const std::string &name,
                               Http_request &request,
                               Http_response &response) const;

  /**
   * Non-signing sync retry. For auth bootstrap calls where the request
   * carries credentials directly (Keystone auth headers, EC2 IMDS
   * tokens) rather than being signed via a signer. Useful when a token
   * refresh hits a transient 5xx mid-backup -- without retry the whole
   * backup aborts. Uses Http_client's own max_retries / max_backoff.
   */
  bool make_request_with_retry(Http_request &request, Http_response &response,
                               const std::string &name) const;

  virtual bool make_async_request(const Http_request &request,
                                  Http_response &response, Event_handler *h,
                                  async_callback_t callback = {},
                                  bool nowait = false) const;
  template <typename CLIENT, typename CALLBACK>
  void callback(CLIENT *client, std::string container, std::string name,
                Http_request *req, Http_response *resp,
                const Http_client *http_client, Event_handler *h,
                CALLBACK callback, CURLcode rc, const Http_connection *conn,
                ulong count) const;
  void set_verbose(bool val) { verbose = val; }
  void set_insecure(bool val) { insecure = val; }
  void set_timeout(ulong val) { timeout = val; }
  void set_cacaert(const std::string &val) { cacert = val; }
  void set_max_retries(ulong val) { max_retries = val; }
  void set_max_backoff(ulong val) { max_backoff = val; }
  ulong get_max_retries() const { return max_retries; }
  ulong get_max_backoff() const { return max_backoff; }
  void set_curl_retriable_errors(CURLcode code) {
    if (code < CURLcode::CURL_LAST) {
      if (std::find(curl_retriable_errors.begin(), curl_retriable_errors.end(),
                    code) == curl_retriable_errors.end()) {
        curl_retriable_errors.push_back(code);
      }
    }
  }
  void set_http_retriable_errors(long error) {
    if (std::find(http_retriable_errors.begin(), http_retriable_errors.end(),
                  error) == http_retriable_errors.end()) {
      http_retriable_errors.push_back(error);
    }
  }
  bool retriable_curl_error(const CURLcode &rc) const;
  bool retriable_http_error(const long &code) const;
  void reset() const { curl = nullptr; }
  virtual bool get_verbose() const { return verbose; }
};

}  // namespace xbcloud

#endif  // XBCLOUD_HTTP_H
