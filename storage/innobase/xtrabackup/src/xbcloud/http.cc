/******************************************************
Copyright (c) 2019, 2023 Percona LLC and/or its affiliates.

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

#include "xbcloud/http.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>

#include "xbcloud/log.h"
#include "xbcloud/util.h"

#include <my_sys.h>
#include <iostream>
#include "azure.h"
#include "common.h"
#include "msg.h"

namespace xbcloud {
namespace stats {
/* Forward declarations of the process-wide observability counters
   defined in multipart.cc. We can't include multipart.h here because
   multipart.h depends on http.h. */
extern std::atomic<uint64_t> total_bytes_appended;
extern std::atomic<uint64_t> total_bytes_uploaded;
extern std::atomic<int> total_parts_inflight;
extern std::atomic<int> total_files_inflight;
}  // namespace stats

namespace http_timing {
Bucket sync_get;
Bucket sync_post;
Bucket sync_put;
Bucket sync_delete;
Bucket sync_head;
std::atomic<bool> enabled{false};

void enable() { enabled.store(true, std::memory_order_relaxed); }

static void dump_one(const char *label, const Bucket &b) {
  uint64_t n = b.calls.load(std::memory_order_relaxed);
  if (n == 0) return;
  uint64_t total = b.total_us.load(std::memory_order_relaxed);
  uint64_t nl = b.namelookup_us.load(std::memory_order_relaxed);
  uint64_t cn = b.connect_us.load(std::memory_order_relaxed);
  uint64_t app = b.appconnect_us.load(std::memory_order_relaxed);
  uint64_t pre = b.pretransfer_us.load(std::memory_order_relaxed);
  uint64_t st = b.starttransfer_us.load(std::memory_order_relaxed);
  uint64_t fresh = b.calls_with_fresh_connect.load(std::memory_order_relaxed);
  char buf[256];
  snprintf(buf, sizeof(buf),
           "  sync %-6s calls=%lu total=%lu ms avg=%.2f ms | "
           "dns=%.2f connect=%.2f tls=%.2f pretx=%.2f startx=%.2f | "
           "fresh_connects=%lu (%.1f%%)",
           label, n, total / 1000, (double)total / 1000.0 / (double)n,
           (double)nl / 1000.0 / (double)n,
           (double)cn / 1000.0 / (double)n,
           (double)app / 1000.0 / (double)n,
           (double)pre / 1000.0 / (double)n,
           (double)st / 1000.0 / (double)n, fresh,
           100.0 * (double)fresh / (double)n);
  log_info() << buf;
}

void dump_summary() {
  if (!enabled.load(std::memory_order_relaxed)) return;
  log_info() << "----- sync HTTP timing summary -----";
  dump_one("GET", sync_get);
  dump_one("POST", sync_post);
  dump_one("PUT", sync_put);
  dump_one("DELETE", sync_delete);
  dump_one("HEAD", sync_head);
  log_info() << "------------------------------------";
}

static Bucket *bucket_for(Http_request::method_t m) {
  switch (m) {
    case Http_request::GET: return &sync_get;
    case Http_request::POST: return &sync_post;
    case Http_request::PUT: return &sync_put;
    case Http_request::DELETE: return &sync_delete;
    case Http_request::HEAD: return &sync_head;
  }
  return nullptr;
}
}  // namespace http_timing
}  // namespace xbcloud
#include "s3.h"
#include "swift.h"

namespace xbcloud {

class Global_curl {
 private:
  CURL *curl{nullptr};
  std::mutex mutex;

 public:
  Global_curl() {}

  bool create() {
    curl = curl_easy_init();
    return (curl != nullptr);
  }

  void cleanup() {
    curl_easy_cleanup(curl);
    curl = nullptr;
  }

  void lock() { mutex.lock(); }

  CURL *get() const { return curl; }

  void unlock() { mutex.unlock(); }
};

static Global_curl global_curl;

bool http_init() {
  curl_global_init(CURL_GLOBAL_ALL);
  return global_curl.create();
}

void http_cleanup() {
  curl_global_cleanup();
  global_curl.cleanup();
}

std::string uri_escape_string(const std::string &s) {
  std::lock_guard<Global_curl> lock(global_curl);
  char *escaped_string =
      curl_easy_escape(global_curl.get(), s.c_str(), s.length());
  std::string result(escaped_string);
  curl_free(escaped_string);
  return result;
}

std::string uri_escape_path(const std::string &path) {
  size_t start = 0;
  size_t end = path.find('/');
  std::string result;

  while (end != std::string::npos) {
    auto part = path.substr(start, end - start);
    result.append(uri_escape_string(part) + '/');
    start = end + 1;
    end = path.find('/', start);
  }
  result.append(uri_escape_string(path.substr(start, end)));

  return result;
}

std::string Http_request::query_string() const {
  std::stringstream query_string;
  int idx = 0;
  /* we need to sort query string params for AWS canonical request */
  for (const auto &param : params()) {
    std::string name = uri_escape_string(param.first);
    std::string val = uri_escape_string(param.second);
    if (idx++ > 0) {
      query_string << "&";
    }
    query_string << name << "=" << val;
  }
  return query_string.str();
}

size_t Http_response::header_appender(char *ptr, size_t size, size_t nmemb,
                                      void *data) {
  size_t buflen = size * nmemb;
  size_t colon_pos = buflen;
  for (size_t i = 0; i < buflen; i++) {
    if (ptr[i] == ':') {
      colon_pos = i;
      break;
    }
  }

  if (colon_pos == buflen) {
    return buflen;
  }

  std::string name(ptr, colon_pos);
  std::string val(ptr + colon_pos + 1, buflen - colon_pos - 1);

  trim(name);
  trim(val);

  to_lower(name);

  if (name.empty()) {
    return buflen;
  }

  Http_response *response = reinterpret_cast<Http_response *>(data);
  response->headers_[name] = val;

  if (name == "content-length") {
    long size = atol(val.c_str());
    if (size > 0 && response->body_.capacity() < (size_t)size) {
      response->body_.reserve(size);
    }
  }

  return buflen;
}

void Event_handler::mcode_or_die(CURLMcode code) {
  if (code == CURLM_OK || code == CURLM_CALL_MULTI_PERFORM) {
    return;
  }
  if (code != CURLM_BAD_SOCKET) {
    assert(0);
  }
}

void Event_handler::remove_socket(Curl_socket_info *socket_info) {
  if (socket_info != nullptr) {
    if (socket_info->evset) {
      ev_io_stop(loop, &socket_info->ev);
    }
    delete socket_info;
  }
}

void Event_handler::set_socket(Curl_socket_info *socket_info,
                               curl_socket_t sockfd, CURL *curl_easy,
                               int action) {
  int kind = (action & (int)CURL_POLL_IN ? (int)EV_READ : 0) |
             (action & (int)CURL_POLL_OUT ? (int)EV_WRITE : 0);

  socket_info->sockfd = sockfd;
  socket_info->action = action;
  socket_info->curl_easy = curl_easy;
  if (socket_info->evset) {
    ev_io_stop(loop, &socket_info->ev);
  }
  ev_io_init(&socket_info->ev, Event_handler::ev_socket_callback,
             socket_info->sockfd, kind);
  socket_info->ev.data = this;
  socket_info->evset = true;
  ev_io_start(loop, &socket_info->ev);
}

void Event_handler::add_socket(curl_socket_t sockfd, CURL *curl_easy,
                               int action) {
  Curl_socket_info *socket_info = new Curl_socket_info();
  set_socket(socket_info, sockfd, curl_easy, action);
  curl_multi_assign(curl_multi, sockfd, socket_info);
}

int Event_handler::multi_timer_callback(CURLM *multi, long timeout_ms,
                                        void *data) {
  Event_handler *h = reinterpret_cast<Event_handler *>(data);

  TRACE("%s %ld\n", __PRETTY_FUNCTION__, timeout_ms);

  ev_timer_stop(h->loop, &h->timer_event);
  if (timeout_ms >= 0) {
    double t = timeout_ms / 1000.;
    ev_timer_init(&h->timer_event, ev_timer_callback, t, 0.);
    ev_timer_start(h->loop, &h->timer_event);
  }
  return 0;
}

int Event_handler::multi_socket_callback(CURL *curl_easy, curl_socket_t sockfd,
                                         int what, void *data,
                                         void *socket_data) {
  Event_handler *h = reinterpret_cast<Event_handler *>(data);
  Curl_socket_info *socket_info =
      reinterpret_cast<Curl_socket_info *>(socket_data);

  TRACE("%s curl_easy %p sockfd %d what %d data %p socket_data %p\n",
        __PRETTY_FUNCTION__, curl_easy, sockfd, what, data, socket_data);

  const char *whatstr[] = {"none", "IN", "OUT", "INOUT", "REMOVE"};

  TRACE("socket callback: sockfd=%d curl_easy=%p what=%s ", sockfd, curl_easy,
        whatstr[what]);

  if (what == CURL_POLL_REMOVE) {
    TRACE("\n");
    h->remove_socket(socket_info);
  } else {
    if (socket_info == nullptr) {
      TRACE("Adding data: %s\n", whatstr[what]);
      h->add_socket(sockfd, curl_easy, what);
    } else {
      TRACE("Changing action from %s to %s\n", whatstr[socket_info->action],
            whatstr[what]);
      h->set_socket(socket_info, sockfd, curl_easy, what);
    }
  }
  return 0;
}

void Event_handler::check_multi_info() {
  int msgs_left{0};
  CURLMsg *msg;

  TRACE("REMAINING: %d\n", running_handles);
  while ((msg = curl_multi_info_read(curl_multi, &msgs_left)) != nullptr) {
    Http_connection *conn;
    if (msg->msg == CURLMSG_DONE) {
      char *url = nullptr;
      auto rc = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &conn);
      assert(rc == CURLE_OK);
      rc = curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL, &url);
      if (rc == CURLE_OK) {
        TRACE("DONE: %s => (%d) %s\n", url, msg->data.result, conn->error());
      }
      conn->finalize(msg->data.result);
      curl_multi_remove_handle(curl_multi, msg->easy_handle);
      delete conn;
      n_queued--;
    }
  }
  if (n_queued < max_requests) {
    process_queue();
  }
}

void Event_handler::ev_socket_callback(EV_P_ struct ev_io *io, int events) {
  int action = (events & (int)EV_READ ? (int)CURL_CSELECT_IN : 0) |
               (events & (int)EV_WRITE ? (int)CURL_CSELECT_OUT : 0);

  TRACE("%s io %p events %d\n", __PRETTY_FUNCTION__, io, events);

  Event_handler *h = reinterpret_cast<Event_handler *>(io->data);

  CURLMcode rc;
  do {
    rc = curl_multi_socket_action(h->curl_multi, io->fd, action,
                                  &h->running_handles);
    mcode_or_die(rc);
  } while (rc == CURLM_CALL_MULTI_PERFORM);

  h->check_multi_info();

  if (h->running_handles <= 0 && h->n_queued == 0) {
    TRACE("last transfer done, kill timeout\n");
    ev_timer_stop(h->loop, &h->timer_event);
  }
}

void Event_handler::ev_timer_callback(EV_P_ struct ev_timer *timer,
                                      int events) {
  Event_handler *h = reinterpret_cast<Event_handler *>(timer->data);

  TRACE("%s timer %p events %d\n", __PRETTY_FUNCTION__, timer, events);

  CURLMcode rc;
  do {
    rc = curl_multi_socket_action(h->curl_multi, CURL_SOCKET_TIMEOUT, 0,
                                  &h->running_handles);
    mcode_or_die(rc);
  } while (rc == CURLM_CALL_MULTI_PERFORM);

  h->check_multi_info();
}

void Event_handler::ev_kickoff_callback(EV_P_ struct ev_timer *timer,
                                        int events) {
  Event_handler *h = reinterpret_cast<Event_handler *>(timer->data);

  TRACE("%s kickoff %p events %d\n", __PRETTY_FUNCTION__, timer, events);

  h->loop_running = true;
}

void Event_handler::ev_queue_callback(EV_P_ ev_async *ev, int revents) {
  Event_handler *h = reinterpret_cast<Event_handler *>(ev->data);
  TRACE("%s async queue callback events %d\n", __PRETTY_FUNCTION__, revents);
  h->process_queue();
}

void Event_handler::process_queue() {
  std::unique_lock<std::mutex> lock(queue_mutex);
  bool popped = false;

  while (!queue.empty() && n_queued < max_requests) {
    auto conn = queue.front();
    TRACE("Adding easy %p to multi %p\n", conn->curl_easy(), conn);
    n_queued++;
    CURLMcode rc = curl_multi_add_handle(curl_multi, conn->curl_easy());
    mcode_or_die(rc);
    queue.pop();
    popped = true;
  }

  if (final && queue.empty()) {
    ev_async_stop(loop, &queue_event);
    if (rate_log_enabled) {
      ev_timer_stop(loop, &rate_log_event);
      rate_log_enabled = false;
    }
  }

  /* PXB-3748: notify producers blocked in add_connection that the queue has
     room. Drop the mutex before notifying to avoid a wake-then-block dance. */
  if (popped) {
    lock.unlock();
    queue_cv.notify_all();
  }
}

bool Event_handler::init() {
  loop = ev_loop_new(0);
  if (loop == nullptr) return false;

  curl_multi = curl_multi_init();
  if (curl_multi == nullptr) return false;

  ev_timer_init(&timer_event, Event_handler::ev_timer_callback, 0., 0.);
  timer_event.data = this;
  ev_timer_start(loop, &timer_event);

  ev_timer_init(&kickoff_event, Event_handler::ev_kickoff_callback, 0.1, 0.);
  kickoff_event.data = this;
  ev_timer_start(loop, &kickoff_event);

  ev_async_init(&queue_event, ev_queue_callback);
  queue_event.data = this;
  ev_async_start(loop, &queue_event);

  curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION,
                    Event_handler::multi_socket_callback);
  curl_multi_setopt(curl_multi, CURLMOPT_SOCKETDATA, this);

  curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION,
                    Event_handler::multi_timer_callback);
  curl_multi_setopt(curl_multi, CURLMOPT_TIMERDATA, this);

  return true;
}

Event_handler::~Event_handler() {
  if (curl_multi != nullptr) curl_multi_cleanup(curl_multi);
  if (loop != nullptr) ev_loop_destroy(loop);
}

void Event_handler::ev_rate_log_callback(EV_P_ struct ev_timer *timer,
                                         int /*events*/) {
  auto *self = static_cast<Event_handler *>(timer->data);

  double now = ev_now(EV_A);
  uint64_t uploaded =
      stats::total_bytes_uploaded.load(std::memory_order_relaxed);
  uint64_t appended =
      stats::total_bytes_appended.load(std::memory_order_relaxed);
  int parts =
      stats::total_parts_inflight.load(std::memory_order_relaxed);
  int files =
      stats::total_files_inflight.load(std::memory_order_relaxed);

  double dt = now - self->rate_log_last_time;
  if (dt < 0.001) dt = 0.001;  /* guard against clock not moving */

  uint64_t d_up = uploaded - self->rate_log_last_uploaded;
  uint64_t d_app = appended - self->rate_log_last_appended;
  double rate_up_mibs = (double)d_up / (1024.0 * 1024.0) / dt;
  double rate_app_mibs = (double)d_app / (1024.0 * 1024.0) / dt;

  char rate_buf[256];
  snprintf(rate_buf, sizeof(rate_buf),
           "rate up=%.1f MiB/s in=%.1f MiB/s (uploaded=%lu MiB "
           "appended=%lu MiB) parts_inflight=%d files=%d",
           rate_up_mibs, rate_app_mibs, uploaded / (1024 * 1024),
           appended / (1024 * 1024), parts, files);
  log_info() << rate_buf;

  self->rate_log_last_time = now;
  self->rate_log_last_uploaded = uploaded;
  self->rate_log_last_appended = appended;
}

void Event_handler::install_rate_logger(double interval_secs) {
  if (interval_secs <= 0.0 || loop == nullptr) return;
  rate_log_enabled = true;
  rate_log_interval = interval_secs;
  rate_log_last_time = ev_now(loop);
  rate_log_last_uploaded =
      stats::total_bytes_uploaded.load(std::memory_order_relaxed);
  rate_log_last_appended =
      stats::total_bytes_appended.load(std::memory_order_relaxed);
  /* Repeating timer: ev_timer fires once after `after` seconds, then
     repeats every `repeat` seconds. Set both to interval. The callback
     runs on the libev thread, sharing this loop with HTTP completions
     -- no new thread, no mutex required around the atomic counters. */
  ev_timer_init(&rate_log_event, Event_handler::ev_rate_log_callback,
                interval_secs, interval_secs);
  rate_log_event.data = this;
  ev_timer_start(loop, &rate_log_event);
}

void Event_handler::main_loop() { ev_loop(loop, 0); }

std::thread Event_handler::run() {
  auto t = std::thread([this]() { main_loop(); });
  while (!loop_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return t;
}

void Event_handler::add_connection(Http_connection *conn, bool nowait) {
  /* PXB-3748: queue depth raised from `max_requests + 4` (which capped
     buffering at ~12 chunks for --parallel=8) to `max_requests * 32`
     (~256 chunks). Allows the producer to keep reading ahead while
     network catches up, avoiding upstream stalls during bursty
     network behavior. The previous 50 ms sleep_for spin-wait is
     replaced by a cv that fires when process_queue pops items. */
  static constexpr size_t QUEUE_DEPTH_MULTIPLIER = 32;
  std::unique_lock<std::mutex> lock(queue_mutex);
  if (!nowait) {
    queue_cv.wait(lock, [this] {
      return queue.size() < max_requests * QUEUE_DEPTH_MULTIPLIER;
    });
  }
  queue.push(conn);
  lock.unlock();
  ev_async_send(loop, &queue_event);
}

void Event_handler::stop() {
  queue_mutex.lock();
  final = true;
  while (true) {
    if (queue.empty()) {
      queue_mutex.unlock();
      ev_async_send(loop, &queue_event);
      break;
    } else {
      queue_mutex.unlock();
      ev_async_send(loop, &queue_event);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool Http_client::retriable_curl_error(const CURLcode &rc) const {
  for (std::vector<CURLcode>::const_iterator it = curl_retriable_errors.begin();
       it != curl_retriable_errors.end(); ++it) {
    if (rc == *it) return true;
  }
  return false;
}

bool Http_client::retriable_http_error(const long &code) const {
  for (std::vector<long>::const_iterator it = http_retriable_errors.begin();
       it != http_retriable_errors.end(); ++it) {
    if (code == *it) return true;
  }
  return false;
}

void Http_client::async_result_callback(async_callback_t user_callback,
                                        Event_handler *h, CURLcode rc,
                                        Http_connection *conn) {
  if (rc != CURLE_OK) {
    log_error() << "Operation failed. Error: " << curl_easy_strerror(rc);
  }
  if (user_callback) {
    user_callback(rc, conn);
  }
}

void Http_client::setup_request(
    CURL *curl, const Http_request &request, Http_response &response,
    curl_slist *&headers, Http_connection::upload_state_t *upload_state) const {
  for (auto &header : request.headers()) {
    headers = curl_slist_append(headers,
                                (header.first + ": " + header.second).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, request.url().c_str());

  switch (request.method()) {
    case Http_request::GET:
      break;
    case Http_request::PUT:
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      upload_state->data = &request.payload()[0];
      upload_state->len = request.payload().size();
      curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                       Http_client::upload_callback);
      curl_easy_setopt(curl, CURLOPT_READDATA, upload_state);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE, upload_state->len);
      break;
    case Http_request::POST:
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      break;
    case Http_request::DELETE:
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
      break;
    case Http_request::HEAD:
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
      break;
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  if (request.method() == Http_request::POST) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, &request.payload()[0]);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     (long)request.payload().size());
  }

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                   Http_response::header_appender);

  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Http_response::body_appender);

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

#if LIBCURL_VERSION_NUM <= 0x071506
  curl_easy_setopt(curl, CURLOPT_ENCODING, "gzip");
#else
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
#endif

  if (verbose) {
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    /* Install our own debug callback so request headers can be
       sanitized before they reach stderr. libcurl's default verbose
       dump includes Authorization (which embeds the AWS access key in
       the Credential= field) and X-Amz-Security-Token (the full STS
       session token). Both must NOT land in customer logs. */
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
                     &Http_client::redacting_debug_callback);
  }

  if (insecure) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  if (!cacert.empty()) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, cacert.c_str());
  }

  if (timeout > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  }
}

int Http_client::upload_callback(char *ptr, size_t size, size_t nmemb,
                                 void *data) {
  Http_connection::upload_state_t *upload =
      reinterpret_cast<Http_connection::upload_state_t *>(data);
  size_t len = std::min(size * nmemb, upload->len);

  TRACE("%s ptr %p size %zu nmemb %zu data %p\n", __PRETTY_FUNCTION__, ptr,
        size, nmemb, data);

  memcpy(ptr, upload->data, len);
  upload->data += len;
  upload->len -= len;

  return len;
}

/* CURLSH lock/unlock callbacks. libcurl calls these around any access
   to shared resources (DNS cache, SSL session cache, connection cache,
   cookies, ...). We use one mutex per resource so unrelated lookups
   don't contend with each other. */
static void share_lock_cb(CURL * /*handle*/, curl_lock_data data,
                          curl_lock_access /*access*/, void *userptr) {
  auto *mutexes = static_cast<std::mutex *>(userptr);
  if (data < 8) mutexes[data].lock();
}

static void share_unlock_cb(CURL * /*handle*/, curl_lock_data data,
                            void *userptr) {
  auto *mutexes = static_cast<std::mutex *>(userptr);
  if (data < 8) mutexes[data].unlock();
}

/* Strip the value from a "Name: value" header line if Name is in the
   sensitive set. Writes the (possibly-redacted) line to stderr with
   the same "=> Send header" tag libcurl uses, so verbose output
   structure looks familiar. Other CURLINFO types are passed through
   verbatim. */
int Http_client::redacting_debug_callback(CURL * /*handle*/,
                                          curl_infotype type, char *data,
                                          size_t size, void * /*userptr*/) {
  /* Header lines we sanitize: any whose value carries credentials. */
  /* Per-provider credential-bearing request headers:
       AWS / S3-compat:  Authorization, X-Amz-Security-Token
       GCS:              Authorization, X-Goog-Session-Token
       Azure:            Authorization (SharedKey signature)
       Swift / Keystone: X-Auth-Token (Keystone bearer token --
                         every Swift request carries it; leaking it
                         is equivalent to leaking the username/password). */
  static const char *const kSensitiveHeaders[] = {
      "authorization",         "x-amz-security-token",
      "x-amz-session-token",   "x-goog-session-token",
      "x-auth-token",          "x-subject-token",
  };

  auto print_prefix = [&](const char *tag) {
    fprintf(stderr, "%s ", tag);
  };

  auto is_sensitive = [&](const char *line, size_t n) -> bool {
    /* Match "name:" prefix, case-insensitive. */
    for (auto *h : kSensitiveHeaders) {
      size_t hlen = strlen(h);
      if (n <= hlen) continue;
      bool match = true;
      for (size_t i = 0; i < hlen; ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c != h[i]) { match = false; break; }
      }
      if (match && line[hlen] == ':') return true;
    }
    return false;
  };

  switch (type) {
    case CURLINFO_HEADER_OUT: {
      /* Outbound request headers -- this is where Authorization and
         X-Amz-Security-Token live. Parse line by line and redact. */
      const char *cur = data;
      const char *end = data + size;
      while (cur < end) {
        const char *eol = static_cast<const char *>(memchr(cur, '\n', end - cur));
        size_t line_len = (eol != nullptr) ? (size_t)(eol - cur + 1)
                                            : (size_t)(end - cur);
        /* Strip trailing CR/LF for the match check. */
        size_t name_len = line_len;
        while (name_len > 0 &&
               (cur[name_len - 1] == '\n' || cur[name_len - 1] == '\r')) {
          --name_len;
        }

        print_prefix("=>");
        if (is_sensitive(cur, name_len)) {
          /* Print "Name: REDACTED" and the original CR/LF. */
          const char *colon =
              static_cast<const char *>(memchr(cur, ':', name_len));
          if (colon != nullptr) {
            fwrite(cur, 1, (size_t)(colon - cur + 1), stderr);
            fputs(" <REDACTED>", stderr);
          }
          for (size_t i = name_len; i < line_len; ++i) fputc(cur[i], stderr);
        } else {
          fwrite(cur, 1, line_len, stderr);
        }
        cur += line_len;
      }
      break;
    }
    case CURLINFO_TEXT:
      print_prefix("*");
      fwrite(data, 1, size, stderr);
      break;
    case CURLINFO_HEADER_IN:
      print_prefix("<=");
      fwrite(data, 1, size, stderr);
      break;
    case CURLINFO_DATA_IN:
    case CURLINFO_DATA_OUT:
    case CURLINFO_SSL_DATA_IN:
    case CURLINFO_SSL_DATA_OUT:
      /* Don't dump bodies (they're large, can contain payload, and
         the previous libcurl default did skip them too for VERBOSE). */
      break;
    default:
      break;
  }
  return 0;
}

void Http_client::init_share() const {
  if (curl_share != nullptr) return;
  curl_share = curl_share_init();
  if (curl_share == nullptr) return;
  curl_share_setopt(curl_share, CURLSHOPT_LOCKFUNC, share_lock_cb);
  curl_share_setopt(curl_share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
  curl_share_setopt(curl_share, CURLSHOPT_USERDATA, &curl_share_mutex[0]);
  /* DNS cache: avoid resolving the upstream on every call. */
  curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  /* TLS session cache: avoid the full TLS handshake on every call. */
  curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#ifdef CURL_LOCK_DATA_CONNECT
  /* Connection cache: reuse the TCP connection across per-call easy
     handles. Available since libcurl 7.57. Crucial on WAN (~3 RTT
     handshake otherwise); near-free on localhost so its impact is
     invisible in the perf_wan.sh harness, but real-AWS runs benefit. */
  curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
#endif
}

bool Http_client::make_request(const Http_request &request,
                               Http_response &response) const {
  CURLcode unused_rc;
  return make_request(request, response, unused_rc);
}

bool Http_client::make_request(const Http_request &request,
                               Http_response &response,
                               CURLcode &out_rc) const {
  curl_slist *headers = nullptr;
  Http_connection::upload_state_t upload_state;

  /* PXB-3671 prototype: use a per-call curl handle so make_request is
     thread-safe. The previous cached `curl` member is shared across the
     Http_client instance and breaks under concurrent multipart-part
     uploads from multiple threads. Allocating per call is a small
     overhead (microseconds) that the multipart-uploaded-in-parallel
     workload easily absorbs. */
  curl_easy_unique_ptr local_curl = make_curl_easy();
  if (!local_curl) {
    msg("error: cannot initialize curl handler\n");
    out_rc = CURLE_FAILED_INIT;
    return false;
  }

  setup_request(local_curl.get(), request, response, headers, &upload_state);

  /* Connect handles to the shared CURLSH pool (if installed) so that
     DNS lookups, SSL sessions, and (with libcurl >= 7.57) connection
     entries are reused across per-call easy handles. Without this, the
     per-call easy handle pattern forces a fresh DNS + TCP + TLS on
     every sync request. */
  if (curl_share != nullptr) {
    curl_easy_setopt(local_curl.get(), CURLOPT_SHARE, curl_share);
  }

  auto res = curl_easy_perform(local_curl.get());
  out_rc = res;
  if (res != CURLE_OK) {
    /* Don't log here unconditionally -- callers (bucket_exists,
       upload_object, ...) already log on their own with proper context.
       Logging here too would print TWO error lines per failure and
       create noise during probe-iteration where some failures are
       expected. Behind --verbose we still emit it for debugging. */
    if (verbose) {
      log_warn() << "http request failed: " << curl_easy_strerror(res);
    }
    curl_slist_free_all(headers);
    return false;
  }

  long http_code;
  curl_easy_getinfo(local_curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

  /* PXB-3671: timing capture. Always cheap (curl already collected the
     numbers); we only aggregate them when http_timing::enabled is true,
     to keep the production path noise-free. */
  if (http_timing::enabled.load(std::memory_order_relaxed)) {
    auto *bucket = http_timing::bucket_for(request.method());
    if (bucket != nullptr) {
      double total_s, nl_s, cn_s, app_s, pre_s, st_s;
      long num_connects = 0;
      curl_easy_getinfo(local_curl.get(), CURLINFO_TOTAL_TIME, &total_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_NAMELOOKUP_TIME, &nl_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_CONNECT_TIME, &cn_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_APPCONNECT_TIME, &app_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_PRETRANSFER_TIME, &pre_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_STARTTRANSFER_TIME, &st_s);
      curl_easy_getinfo(local_curl.get(), CURLINFO_NUM_CONNECTS, &num_connects);
      bucket->calls.fetch_add(1, std::memory_order_relaxed);
      bucket->total_us.fetch_add((uint64_t)(total_s * 1.0e6),
                                  std::memory_order_relaxed);
      bucket->namelookup_us.fetch_add((uint64_t)(nl_s * 1.0e6),
                                       std::memory_order_relaxed);
      bucket->connect_us.fetch_add((uint64_t)(cn_s * 1.0e6),
                                    std::memory_order_relaxed);
      bucket->appconnect_us.fetch_add((uint64_t)(app_s * 1.0e6),
                                       std::memory_order_relaxed);
      bucket->pretransfer_us.fetch_add((uint64_t)(pre_s * 1.0e6),
                                        std::memory_order_relaxed);
      bucket->starttransfer_us.fetch_add((uint64_t)(st_s * 1.0e6),
                                          std::memory_order_relaxed);
      if (num_connects > 0) {
        bucket->calls_with_fresh_connect.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
  }

  curl_slist_free_all(headers);

  response.set_http_code(http_code);
  return true;
}

template <typename CLIENT>
bool Http_client::make_request_with_retry(CLIENT *client,
                                          const std::string &container,
                                          const std::string &name,
                                          Http_request &request,
                                          Http_response &response) const {
  ulong count = 0;
  while (true) {
    /* (Re)sign the request with the current timestamp. The signer
       removes any previously-set Date/Authorization headers, so this
       is idempotent across retries. */
    client->signer->sign_request(client->hostname(container), container,
                                 request, time(0));

    CURLcode rc;
    response.reset_body();
    bool transport_ok = make_request(request, response, rc);

    bool retry_error = false;
    if (!transport_ok) {
      if (retriable_curl_error(rc)) {
        retry_error = true;
      } else if (get_verbose()) {
        log_warn() << "Curl error (" << rc << ") " << curl_easy_strerror(rc)
                   << " is not configured as retriable. You can allow it "
                   << "by adding --curl-retriable-errors=" << rc
                   << " parameter";
      }
    } else if (retriable_http_error(response.http_code())) {
      retry_error = true;
    } else if (!response.ok()) {
      /* Some providers signal retriable conditions in the body even
         though the HTTP status itself is not in the retriable list
         (e.g. S3's SlowDown / RequestTimeout). */
      client->retry_error(&response, &retry_error);
    }

    if (!retry_error) return transport_ok;
    if (count >= client->get_max_retries()) {
      log_error() << "No more retries for " << name;
      return false;
    }

    ulong delay = get_exponential_backoff(count + 1, client->get_max_backoff());
    log_info() << "Sleeping for " << delay << " ms before retrying " << name
               << " [" << (count + 1) << "]";
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    ++count;
  }
}

bool Http_client::make_async_request(const Http_request &request,
                                     Http_response &response, Event_handler *h,
                                     async_callback_t callback,
                                     bool nowait) const {
  curl_slist *headers = nullptr;

  auto curl = make_curl_easy();
  if (!curl) {
    msg("error: cannot initialize curl handler\n");
    return false;
  }

  using std::placeholders::_1;
  using std::placeholders::_2;

  auto cb = std::bind(async_result_callback, callback, h, _1, _2);

  auto conn = new Http_connection(std::move(curl), request, response, cb);
  setup_request(conn->curl_easy(), request, response, headers,
                conn->upload_state());
  if (curl_share != nullptr) {
    curl_easy_setopt(conn->curl_easy(), CURLOPT_SHARE, curl_share);
  }

  conn->set_headers(headers);
  h->add_connection(conn, nowait);

  return true;
}

template <typename CLIENT, typename CALLBACK>
void Http_client::callback(CLIENT *client, std::string container,
                           std::string name, Http_request *req,
                           Http_response *resp, const Http_client *http_client,
                           Event_handler *h, CALLBACK callback, CURLcode rc,
                           const Http_connection *conn, ulong count) const {
  bool retry_error = false;

  if (http_client->retriable_curl_error(rc)) {
    retry_error = true;
  } else if (!retry_error && rc != CURLE_OK && http_client->get_verbose()) {
    log_warn() << "Curl error (" << rc << ") " << curl_easy_strerror(rc)
               << " is not configured as retriable. You can allow it by "
               << "adding --curl-retriable-errors=" << rc << " parameter";
  }
  if (http_client->retriable_http_error(conn->response().http_code())) {
    retry_error = true;
  } else if (!retry_error && rc != CURLE_OK && http_client->get_verbose()) {
    log_warn() << "http error (" << conn->response().http_code()
               << ") is not configured as retriable. You can allow it by "
               << "adding --http-retriable-errors="
               << conn->response().http_code() << " parameter";
  }
  if (rc == CURLE_OK && !resp->ok()) {
    client->retry_error(resp, &retry_error);
  }

  if (retry_error && count <= client->get_max_retries()) {
    ulong delay = get_exponential_backoff(count, client->get_max_backoff());
    log_info() << "Sleeping for " << delay << " ms before retrying " << name
               << " [" << count << "]";
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    resp->reset_body();
    client->signer->sign_request(client->hostname(container), container, *req,
                                 time(0));
    http_client->make_async_request(
        *req, *resp, h,
        std::bind(&Http_client::callback<CLIENT, CALLBACK>, this, client,
                  container, name, req, resp, http_client, h, callback,
                  std::placeholders::_1, std::placeholders::_2, count + 1),
        true);
    return;
  } else if (retry_error && count > client->get_max_retries())
    log_error() << "No more retries for " << name;

  if (callback) {
    callback(rc == CURLE_OK && resp->ok(), resp->body());
  }
  delete req;
  delete resp;
}

/* async_download_callback_t and async_upload_callback_t are been resolved as
 * the same function by the compiler, thus no need to re-declare the function
 * signature here */

template void
Http_client::callback<Swift_client, Swift_client::async_download_callback_t>(
    Swift_client *client, std::string container, std::string name,
    Http_request *req, Http_response *resp, const Http_client *http_client,
    Event_handler *h, Swift_client::async_download_callback_t callback,
    CURLcode rc, const Http_connection *conn, ulong count) const;

template void
Http_client::callback<S3_client, S3_client::async_download_callback_t>(
    S3_client *client, std::string container, std::string name,
    Http_request *req, Http_response *resp, const Http_client *http_client,
    Event_handler *h, S3_client::async_download_callback_t callback,
    CURLcode rc, const Http_connection *conn, ulong count) const;

template void
Http_client::callback<Azure_client, Azure_client::async_download_callback_t>(
    Azure_client *client, std::string container, std::string name,
    Http_request *req, Http_response *resp, const Http_client *http_client,
    Event_handler *h, Azure_client::async_download_callback_t callback,
    CURLcode rc, const Http_connection *conn, ulong count) const;

template bool Http_client::make_request_with_retry<S3_client>(
    S3_client *, const std::string &, const std::string &, Http_request &,
    Http_response &) const;
template bool Http_client::make_request_with_retry<Azure_client>(
    Azure_client *, const std::string &, const std::string &, Http_request &,
    Http_response &) const;
template bool Http_client::make_request_with_retry<Swift_client>(
    Swift_client *, const std::string &, const std::string &, Http_request &,
    Http_response &) const;

bool Http_client::make_request_with_retry(Http_request &request,
                                          Http_response &response,
                                          const std::string &name) const {
  ulong count = 0;
  while (true) {
    CURLcode rc;
    response.reset_body();
    bool transport_ok = make_request(request, response, rc);

    bool retry_error = false;
    if (!transport_ok) {
      if (retriable_curl_error(rc)) retry_error = true;
    } else if (retriable_http_error(response.http_code())) {
      retry_error = true;
    }

    if (!retry_error) return transport_ok;
    if (count >= max_retries) {
      log_error() << "No more retries for " << name;
      return false;
    }

    ulong delay = get_exponential_backoff(count + 1, max_backoff);
    log_info() << "Sleeping for " << delay << " ms before retrying " << name
               << " [" << (count + 1) << "]";
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    ++count;
  }
}
}  // namespace xbcloud
