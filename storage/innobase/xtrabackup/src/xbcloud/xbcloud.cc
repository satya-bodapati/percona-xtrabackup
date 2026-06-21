/******************************************************
Copyright (c) 2014, 2023 Percona LLC and/or its affiliates.

xbcloud utility. Manage backups on cloud storage services.

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

#include <my_alloc.h>
#include <my_default.h>
#include <my_dir.h>
#include <my_getopt.h>
#include <my_sys.h>
#include <my_thread_local.h>
#include <mysql/service_mysql_alloc.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <typelib.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <set>
#include <string>
#include <unordered_map>
#include "template_utils.h"

#include <curl/curl.h>

#include "common.h"
#include "file_utils.h"
#include "xbstream.h"
#include "xtrabackup_version.h"

#include "crc_glue.h"
#include "library_version_check.h"
#include "msg.h"
#include "nulls.h"
#include "xbcloud/azure.h"
#include "xbcloud/multipart.h"
#include "xbcloud/s3.h"
#include "xbcloud/s3_ec2.h"
#include "xbcloud/swift.h"
#include "xbcloud/util.h"
#include "xbcloud/xbcloud.h"
#include "xbcrypt_common.h"

using namespace xbcloud;

#define XBCLOUD_VERSION XTRABACKUP_VERSION
#define XBCLOUD_REVISION XTRABACKUP_REVISION

/*****************************************************************************/

const char *config_file = "my"; /* Default config file */

const static int chunk_index_prefix_len = 20;

const char *azure_development_access_key =
    "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/"
    "KBHBeksoGMGw==";
const char *azure_development_storage_account = "devstoreaccount1";
const char *azure_development_container = "testcontainer";

enum { SWIFT, S3, GOOGLE, AZURE };
const char *storage_names[] = {"SWIFT", "S3", "GOOGLE", "AZURE", NullS};

const char *s3_bucket_lookup_names[] = {"AUTO", "DNS", "PATH", NullS};

static bool opt_verbose = 0;

static ulong opt_storage = SWIFT;

static char *opt_swift_user = nullptr;
static char *opt_swift_user_id = nullptr;
static char *opt_swift_password = nullptr;
static char *opt_swift_tenant = nullptr;
static char *opt_swift_tenant_id = nullptr;
static char *opt_swift_project = nullptr;
static char *opt_swift_project_id = nullptr;
static char *opt_swift_domain = nullptr;
static char *opt_swift_domain_id = nullptr;
static char *opt_swift_project_domain = nullptr;
static char *opt_swift_project_domain_id = nullptr;
static char *opt_swift_region = nullptr;
static char *opt_swift_container = nullptr;
static char *opt_swift_storage_url = nullptr;
static char *opt_swift_auth_url = nullptr;
static char *opt_swift_key = nullptr;
static char *opt_swift_auth_version = nullptr;

static char *opt_s3_region = nullptr;
static char *opt_s3_endpoint = nullptr;
static char *opt_s3_access_key = nullptr;
static char *opt_s3_secret_key = nullptr;
static char *opt_s3_session_token = nullptr;
static char *opt_s3_storage_class = nullptr;
static char *opt_s3_bucket = nullptr;
static ulong opt_s3_bucket_lookup;
static ulong opt_s3_api_version = 0;

static char *opt_google_region = nullptr;
static char *opt_google_endpoint = nullptr;
static char *opt_google_access_key = nullptr;
static char *opt_google_secret_key = nullptr;
static char *opt_google_session_token = nullptr;
static char *opt_google_storage_class = nullptr;
static char *opt_google_bucket = nullptr;

static char *opt_azure_account = nullptr;
static char *opt_azure_container = nullptr;
static char *opt_azure_access_key = nullptr;
static char *opt_azure_endpoint = nullptr;
static char *opt_azure_storage_class = nullptr;
static bool opt_azure_development_storage = 0;

static std::string backup_name;
static char *opt_cacert = nullptr;
static ulong opt_parallel = 1;
static ulong opt_threads = 1;
static char *opt_fifo_dir = nullptr;
static ulong opt_fifo_timeout = 60;
static ulong opt_timeout = 120;
static ulong opt_max_retries = 10;
static u_int32_t opt_max_backoff = 300000;

static bool opt_insecure = false;
static bool opt_md5 = false;

/* PXB-3671 prototype: when true, the PUT path uploads each logical file as
   one object using backend multipart upload (one object per file, no .NNNNN
   chunk suffix). When false, the legacy chunk-per-PUT path is used. */
static bool opt_multipart_upload = true;

/* PXB-3671 prototype: target multipart part size. When 0 (default), the
   tiered dynamic_part_size() schedule picks the part size based on bytes
   uploaded so far (streaming) or the known file_size (--multipart-from-file).
   A non-zero value overrides the schedule with a fixed per-part target;
   the final part for a file may still be smaller. */
static ulonglong opt_multipart_part_size = 0;

/* PXB-3671 prototype: peak in-flight buffer per file. Multipart_uploader
   blocks the producer when admitting a new part would push total bytes
   pending in libcurl-multi past this. Tunes throughput vs RAM:
   4 GiB allows ~256 in flight at 16 MiB parts (early streaming) or
   ~6 in flight at 600 MiB parts (tail of a >1 TiB stream). */
static ulonglong opt_multipart_memory_budget = 4ULL * 1024 * 1024 * 1024;

/* PXB-3671 prototype: streams that turn out to be smaller than this at
   EOF (no parts submitted yet, total accumulated buffer below threshold)
   bypass multipart and ship as a single PUT. Avoids the
   InitiateMultipartUpload + UploadPart + CompleteMultipartUpload round-
   trip overhead for small metadata files. */
static ulonglong opt_multipart_threshold = 16ULL * 1024 * 1024;

/* PXB-3671 prototype: per-call timing instrumentation for sync HTTP.
   When on, every Http_client::make_request() records its curl phase
   timings (DNS, CONNECT, TLS, pretransfer, total) into a per-method
   bucket; xbcloud dumps a summary at shutdown. Used to diagnose the
   multipart-vs-legacy WAN regression -- in particular to confirm
   whether per-call easy handles are forcing fresh TCP+TLS handshakes
   on every sync request (the CURLSH share fix hinges on this signal). */
static bool opt_http_timing = false;

/* PXB-3671 prototype: periodic throughput logging. Every N seconds the
   Event_handler's libev timer fires and logs the current upload rate
   (MiB/s) and in-flight part / file counts. 0 disables. Runs on the
   existing Event_handler thread -- no new thread is spawned. */
static ulong opt_rate_log_interval = 10;

/* PXB-3671 prototype: per-object rollover threshold. Files larger than
   this in --multipart-from-file mode are split into multiple objects
   named <name>.part-001, <name>.part-002, ... with a sidecar manifest
   at <name>.manifest.json listing the segments. Default is 5 TiB
   (S3's per-object hard cap); knob is exposed so smoke tests can drop
   it (e.g. 256 MiB) and exercise the rollover path on small files. */
static ulonglong opt_multipart_rollover_threshold =
    5ULL * 1024 * 1024 * 1024 * 1024;

/* PXB-3671 prototype: local file source for multipart upload. When set,
   xbcloud reads this file from disk, picks part_size via the tiered
   schedule, and uploads via multipart. Bypasses xbstream entirely. */
static char *opt_multipart_from_file = nullptr;

static enum { MODE_GET, MODE_PUT, MODE_DELETE, MODE_PROBE } opt_mode;

static std::map<std::string, std::string> extra_http_headers;

const char *s3_api_version_names[] = {"AUTO", "2", "4", NullS};

/* list of partial files to be downloaded or delete*/
static std::set<std::string> partial_file_list;

TYPELIB storage_typelib = {array_elements(storage_names) - 1, "", storage_names,
                           nullptr};

TYPELIB s3_bucket_lookup_typelib = {array_elements(s3_bucket_lookup_names) - 1,
                                    "", s3_bucket_lookup_names, nullptr};

TYPELIB s3_api_version_typelib = {array_elements(s3_api_version_names) - 1, "",
                                  s3_api_version_names, nullptr};

Http_client http_client;

enum {
  OPT_STORAGE = 256,

  OPT_SWIFT_CONTAINER,
  OPT_SWIFT_AUTH_URL,
  OPT_SWIFT_KEY,
  OPT_SWIFT_USER,
  OPT_SWIFT_USER_ID,
  OPT_SWIFT_PASSWORD,
  OPT_SWIFT_TENANT,
  OPT_SWIFT_TENANT_ID,
  OPT_SWIFT_PROJECT,
  OPT_SWIFT_PROJECT_ID,
  OPT_SWIFT_DOMAIN,
  OPT_SWIFT_DOMAIN_ID,
  OPT_SWIFT_PROJECT_DOMAIN,
  OPT_SWIFT_PROJECT_DOMAIN_ID,
  OPT_SWIFT_REGION,
  OPT_SWIFT_STORAGE_URL,
  OPT_SWIFT_AUTH_VERSION,

  OPT_S3_REGION,
  OPT_S3_ENDPOINT,
  OPT_S3_ACCESS_KEY,
  OPT_S3_SECRET_KEY,
  OPT_S3_SESSION_TOKEN,
  OPT_S3_STORAGE_CLASS,
  OPT_S3_BUCKET,
  OPT_S3_BUCKET_LOOKUP,
  OPT_S3_API_VERSION,

  OPT_AZURE_ACCOUNT,
  OPT_AZURE_CONTAINER,
  OPT_AZURE_ACCESS_KEY,
  OPT_AZURE_STORAGE_CLASS,
  OPT_AZURE_ENDPOINT,
  OPT_AZURE_DEVELOPMENT_STORAGE,

  OPT_GOOGLE_REGION,
  OPT_GOOGLE_ENDPOINT,
  OPT_GOOGLE_ACCESS_KEY,
  OPT_GOOGLE_SECRET_KEY,
  OPT_GOOGLE_SESSION_TOKEN,
  OPT_GOOGLE_STORAGE_CLASS,
  OPT_GOOGLE_BUCKET,

  OPT_PARALLEL,
  OPT_THREADS,
  OPT_FIFO_DIR,
  OPT_FIFO_TIMEOUT,
  OPT_TIMEOUT,
  OPT_MAX_RETRIES,
  OPT_MAX_BACKOFF,
  OPT_CACERT,
  OPT_HEADER,
  OPT_INSECURE,
  OPT_MD5,
  OPT_VERBOSE,
  OPT_CURL_RETRIABLE_ERRORS,
  OPT_HTTP_RETRIABLE_ERRORS,
  OPT_MULTIPART_UPLOAD,
  OPT_MULTIPART_PART_SIZE,
  OPT_MULTIPART_MEMORY_BUDGET,
  OPT_MULTIPART_THRESHOLD,
  OPT_MULTIPART_ROLLOVER_THRESHOLD,
  OPT_RATE_LOG_INTERVAL,
  OPT_HTTP_TIMING,
  OPT_MULTIPART_FROM_FILE
};

static struct my_option my_long_options[] = {
    {"defaults-file", 'c',
     "Name of config file to read; if no extension is given, default "
     "extension (e.g., .ini or .cnf) will be added",
     &config_file, &config_file, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG, 0,
     0, 0, 0, 0, 0},

    {"version", 'V', "Display version and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG,
     0, 0, 0, 0, 0, 0},

    {"storage", OPT_STORAGE, "Specify storage type S3/SWIFT/GOOGLE/AZURE.",
     &opt_storage, &opt_storage, &storage_typelib, GET_ENUM, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"swift-auth-version", OPT_SWIFT_AUTH_VERSION,
     "Swift authentication verison to use.", &opt_swift_auth_version,
     &opt_swift_auth_version, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-container", OPT_SWIFT_CONTAINER,
     "Swift container to store backups into.", &opt_swift_container,
     &opt_swift_container, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-user", OPT_SWIFT_USER, "Swift user name.", &opt_swift_user,
     &opt_swift_user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-user-id", OPT_SWIFT_USER_ID, "Swift user ID.", &opt_swift_user_id,
     &opt_swift_user_id, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-auth-url", OPT_SWIFT_AUTH_URL,
     "Base URL of SWIFT authentication service.", &opt_swift_auth_url,
     &opt_swift_auth_url, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-storage-url", OPT_SWIFT_STORAGE_URL,
     "URL of object-store endpoint. Usually received from authentication "
     "service. Specify to override this value.",
     &opt_swift_storage_url, &opt_swift_storage_url, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-key", OPT_SWIFT_KEY, "Swift key.", &opt_swift_key, &opt_swift_key,
     0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-tenant", OPT_SWIFT_TENANT,
     "The tenant name. Both the --swift-tenant and --swift-tenant-id "
     "options are optional, but should not be specified together.",
     &opt_swift_tenant, &opt_swift_tenant, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"swift-tenant-id", OPT_SWIFT_TENANT_ID,
     "The tenant ID. Both the --swift-tenant and --swift-tenant-id "
     "options are optional, but should not be specified together.",
     &opt_swift_tenant_id, &opt_swift_tenant_id, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"swift-project", OPT_SWIFT_PROJECT, "The project name.",
     &opt_swift_project, &opt_swift_project, 0, GET_STR_ALLOC, REQUIRED_ARG, 0,
     0, 0, 0, 0, 0},

    {"swift-project-id", OPT_SWIFT_PROJECT_ID, "The project ID.",
     &opt_swift_project_id, &opt_swift_project_id, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"swift-domain", OPT_SWIFT_DOMAIN, "The user domain name.",
     &opt_swift_domain, &opt_swift_domain, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"swift-domain-id", OPT_SWIFT_DOMAIN_ID, "The user domain ID.",
     &opt_swift_domain_id, &opt_swift_domain_id, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"swift-project-domain", OPT_SWIFT_PROJECT_DOMAIN,
     "The project domain name.", &opt_swift_project_domain,
     &opt_swift_project_domain, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0,
     0},

    {"swift-project-domain-id", OPT_SWIFT_PROJECT_DOMAIN_ID,
     "The project domain ID.", &opt_swift_project_domain_id,
     &opt_swift_project_domain_id, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0,
     0, 0},

    {"swift-password", OPT_SWIFT_PASSWORD, "The password of the user.",
     &opt_swift_password, &opt_swift_password, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"swift-region", OPT_SWIFT_REGION, "The region object-store endpoint.",
     &opt_swift_region, &opt_swift_region, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0,
     0, 0, 0, 0},

    {"parallel", OPT_PARALLEL, "Number of parallel chunk uploads.",
     &opt_parallel, &opt_parallel, 0, GET_ULONG, REQUIRED_ARG, 1, 1, ULONG_MAX,
     0, 0, 0},

    {"fifo-streams", OPT_THREADS, "Number of parallel fifo stream threads.",
     &opt_threads, &opt_threads, 0, GET_ULONG, REQUIRED_ARG, 1, 1, ULONG_MAX, 0,
     0, 0},

    {"fifo-dir", OPT_FIFO_DIR,
     "Directory to read/write Named Pipe. On put mode, xbcloud read from named "
     "pipes. On get mode, xbcloud writes to named pipes.",
     &opt_fifo_dir, &opt_fifo_dir, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0,
     0, 0},

    {"fifo-timeout", OPT_FIFO_TIMEOUT,
     "How many seconds to wait for other end to open the stream. "
     "Default 60 seconds",
     &opt_fifo_timeout, &opt_fifo_timeout, 0, GET_INT, REQUIRED_ARG, 60, 1,
     INT_MAX, 0, 0, 0},

    {"timeout", OPT_TIMEOUT,
     "How many seconds to wait for activity on TCP connection. Setting this to "
     "0 means no timeout. Default 120 seconds",
     &opt_timeout, &opt_timeout, 0, GET_INT, REQUIRED_ARG, 120, 0, INT_MAX, 0,
     0, 0},

    {"max-retries", OPT_MAX_RETRIES,
     "Number of retries of chunk uploads/downloads after a failure (Default "
     "10).",
     &opt_max_retries, &opt_max_retries, 0, GET_ULONG, REQUIRED_ARG, 10, 1,
     ULONG_MAX, 0, 0, 0},

    {"max-backoff", OPT_MAX_BACKOFF,
     "Maximum backoff delay in milliseconds in between chunk uploads/downloads "
     "retries "
     "(Default 300000).",
     &opt_max_backoff, &opt_max_backoff, 0, GET_UINT32, REQUIRED_ARG, 300000, 1,
     UINT_MAX32, 0, 0, 0},

    {"s3-region", OPT_S3_REGION, "S3 region.", &opt_s3_region, &opt_s3_region,
     0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-endpoint", OPT_S3_ENDPOINT, "S3 endpoint.", &opt_s3_endpoint,
     &opt_s3_endpoint, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-access-key", OPT_S3_ACCESS_KEY, "S3 access key.", &opt_s3_access_key,
     &opt_s3_access_key, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-secret-key", OPT_S3_SECRET_KEY, "S3 secret key.", &opt_s3_secret_key,
     &opt_s3_secret_key, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-session-token", OPT_S3_SESSION_TOKEN, "S3 session token.",
     &opt_s3_session_token, &opt_s3_session_token, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-storage-class", OPT_S3_STORAGE_CLASS,
     "S3 storage class. STANDARD|STANDARD_IA|GLACIER|...     "
     "... is meant for passing "
     "custom storage class names provided by other S3 implementations such as "
     "MinIO CephRadosGW, etc.",
     &opt_s3_storage_class, &opt_s3_storage_class, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-bucket", OPT_S3_BUCKET, "S3 bucket.", &opt_s3_bucket, &opt_s3_bucket,
     0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-bucket-lookup", OPT_S3_BUCKET_LOOKUP, "Bucket lookup method.",
     &opt_s3_bucket_lookup, &opt_s3_bucket_lookup, &s3_bucket_lookup_typelib,
     GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"s3-api-version", OPT_S3_API_VERSION, "S3 API version.",
     &opt_s3_api_version, &opt_s3_api_version, &s3_api_version_typelib,
     GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"azure-storage-account", OPT_AZURE_ACCOUNT, "AZURE storage account. ",
     &opt_azure_account, &opt_azure_account, 0, GET_STR_ALLOC, REQUIRED_ARG, 0,
     0, 0, 0, 0, 0},

    {"azure-container-name", OPT_AZURE_CONTAINER, "AZURE container name. ",
     &opt_azure_container, &opt_azure_container, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"azure-access-key", OPT_AZURE_ACCESS_KEY, "AZURE access key.",
     &opt_azure_access_key, &opt_azure_access_key, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"azure-development-storage", OPT_AZURE_DEVELOPMENT_STORAGE,
     "To run against azurite emulator use --azure-development-storage. It can "
     "work with the default credentials provided by azurite. For example, it "
     "uses http://127.0.0.1:10000 as the default endpoint, which can be "
     "overwritten by --azure-endpoint. Users can also provide "
     "--azure-access-key,  --azure-storage-account, --azure-container-name",
     &opt_azure_development_storage, &opt_azure_development_storage, 0,
     GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"azure-endpoint", OPT_AZURE_ENDPOINT, "Azure cloud storage endpoint.",
     &opt_azure_endpoint, &opt_azure_endpoint, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"azure-tier-class", OPT_AZURE_STORAGE_CLASS,
     "Azure cloud tier class. Hot|Cool|Archive", &opt_azure_storage_class,
     &opt_azure_storage_class, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0,
     0},

    {"google-region", OPT_GOOGLE_REGION, "Google cloud storage region.",
     &opt_google_region, &opt_google_region, 0, GET_STR_ALLOC, REQUIRED_ARG, 0,
     0, 0, 0, 0, 0},

    {"google-endpoint", OPT_GOOGLE_ENDPOINT, "Google cloud storage endpoint.",
     &opt_google_endpoint, &opt_google_endpoint, 0, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {"google-access-key", OPT_GOOGLE_ACCESS_KEY,
     "Google cloud storage access key.", &opt_google_access_key,
     &opt_google_access_key, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"google-secret-key", OPT_GOOGLE_SECRET_KEY,
     "Google cloud storage secret key.", &opt_google_secret_key,
     &opt_google_secret_key, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"google-session-token", OPT_GOOGLE_SESSION_TOKEN,
     "Google cloud storage session token.", &opt_google_session_token,
     &opt_google_session_token, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0,
     0},

    {"google-storage-class", OPT_GOOGLE_STORAGE_CLASS,
     "Google cloud storage class. STANDARD|NEARLINE|COLDLINE|ARCHIVE",
     &opt_google_storage_class, &opt_google_storage_class, 0, GET_STR_ALLOC,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"google-bucket", OPT_GOOGLE_BUCKET, "Google cloud storage bucket.",
     &opt_google_bucket, &opt_google_bucket, 0, GET_STR_ALLOC, REQUIRED_ARG, 0,
     0, 0, 0, 0, 0},

    {"cacert", OPT_CACERT, "CA certificate file.", &opt_cacert, &opt_cacert, 0,
     GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"header", OPT_HEADER, "Extra header.", NULL, NULL, 0, GET_STR,
     REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"insecure", OPT_INSECURE, "Do not verify server SSL certificate.",
     &opt_insecure, &opt_insecure, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"md5", OPT_MD5, "Upload MD5 file into the backup dir.", &opt_md5, &opt_md5,
     0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"verbose", OPT_VERBOSE, "Turn ON cURL tracing.", &opt_verbose,
     &opt_verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

    {"curl-retriable-errors", OPT_CURL_RETRIABLE_ERRORS,
     "Add a new curl error code as retriable. For multiple codes, use a comma "
     "separated list of codes.",
     0, 0, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"http-retriable-errors", OPT_HTTP_RETRIABLE_ERRORS,
     "Add a new http error code as retriable. For multiple codes, use a comma "
     "separated list of codes.",
     0, 0, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

    {"multipart-upload", OPT_MULTIPART_UPLOAD,
     "PXB-3671 prototype: upload each logical file as a single object using "
     "the backend's multipart upload API, producing one object per file with "
     "the real file name. When OFF, the legacy per-chunk PUT path is used.",
     &opt_multipart_upload, &opt_multipart_upload, 0, GET_BOOL, NO_ARG, 1, 0, 0,
     0, 0, 0},

    {"multipart-part-size", OPT_MULTIPART_PART_SIZE,
     "PXB-3671 prototype: override the dynamic part-size schedule with a "
     "fixed part size in bytes. When 0 (default), the tiered schedule "
     "ramps from 16 MiB to 600 MiB based on bytes uploaded so far "
     "(streaming) or stat'd file size (--multipart-from-file).",
     &opt_multipart_part_size, &opt_multipart_part_size, 0, GET_ULL,
     REQUIRED_ARG, 0, 0,
     5ULL * 1024 * 1024 * 1024, 0, 0, 0},

    {"multipart-memory-budget", OPT_MULTIPART_MEMORY_BUDGET,
     "PXB-3671 prototype: peak in-flight buffer size per file in bytes. "
     "Producer blocks when admitting a new part would push pending "
     "multipart traffic past this. Default 4 GiB.",
     &opt_multipart_memory_budget, &opt_multipart_memory_budget, 0, GET_ULL,
     REQUIRED_ARG, 4ULL * 1024 * 1024 * 1024, 16ULL * 1024 * 1024,
     ULLONG_MAX, 0, 0, 0},

    {"multipart-threshold", OPT_MULTIPART_THRESHOLD,
     "PXB-3671 prototype: streams that finish below this size with no "
     "parts submitted yet ship as a single PUT instead of going through "
     "multipart. Default 16 MiB. Set to 0 to force multipart for every "
     "stream.",
     &opt_multipart_threshold, &opt_multipart_threshold, 0, GET_ULL,
     REQUIRED_ARG, 16ULL * 1024 * 1024, 0,
     5ULL * 1024 * 1024 * 1024, 0, 0, 0},

    {"http-timing", OPT_HTTP_TIMING,
     "PXB-3671 prototype: record per-call curl phase timings (DNS, "
     "CONNECT, TLS, pretransfer, total) for every sync make_request() "
     "and dump an aggregated summary at shutdown. Used to diagnose "
     "where WAN latency is going (fresh TCP+TLS per call vs reused).",
     &opt_http_timing, &opt_http_timing, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0,
     0},

    {"rate-log-interval", OPT_RATE_LOG_INTERVAL,
     "PXB-3671 prototype: log upload throughput every N seconds via the "
     "existing Event_handler libev timer (no extra thread). Reports "
     "goodput (delivered bytes / wall time) -- retried bytes are NOT "
     "double-counted because the counter only increments on a part's "
     "successful completion, not on wire transmission. Set to 0 to "
     "disable. Default 10.",
     &opt_rate_log_interval, &opt_rate_log_interval, 0, GET_ULONG,
     REQUIRED_ARG, 10, 0, 86400, 0, 0, 0},

    {"multipart-rollover-threshold", OPT_MULTIPART_ROLLOVER_THRESHOLD,
     "PXB-3671 prototype: per-object rollover threshold. Files larger "
     "than this in --multipart-from-file mode split into "
     "<name>.part-001/.part-002/... with a <name>.manifest.json sidecar. "
     "Default is 5 TiB (S3 per-object hard cap). Lower it for testing.",
     &opt_multipart_rollover_threshold, &opt_multipart_rollover_threshold, 0,
     GET_ULL, REQUIRED_ARG, 5ULL * 1024 * 1024 * 1024 * 1024,
     16ULL * 1024 * 1024, ULLONG_MAX, 0, 0, 0},

    {"multipart-from-file", OPT_MULTIPART_FROM_FILE,
     "PXB-3671 prototype: upload a local file via multipart upload, "
     "bypassing xbstream entirely. Path to a local file. xbcloud stats "
     "the file, picks the part_size via dynamic_part_size(file_size), "
     "then multipart-uploads it to the given backup-name/<basename> key.",
     &opt_multipart_from_file, &opt_multipart_from_file, 0, GET_STR, REQUIRED_ARG,
     0, 0, 0, 0, 0, 0},

    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};

static void print_version() {
  printf("%s  Ver %s%s for %s (%s) (revision id: %s)\n", my_progname,
         XBCLOUD_VERSION, get_suffix_str().c_str(), SYSTEM_TYPE, MACHINE_TYPE,
         XBCLOUD_REVISION);
}

static void usage() {
  print_version();
  puts("Copyright (C) 2015, 2021 Percona LLC and/or its affiliates.");
  puts(
      "This software comes with ABSOLUTELY NO WARRANTY. "
      "This is free software,\nand you are welcome to modify and "
      "redistribute it under the GPL license.\n");

  puts("Manage backups on the cloud services.\n");

  puts("Usage: ");
  printf(
      "  %s put [OPTIONS...] <NAME> upload backup from STDIN into "
      "the cloud service with given name.\n",
      my_progname);
  printf(
      "  %s get [OPTIONS...] <NAME> [FILES...] stream specified "
      "backup or individual files from the cloud service into STDOUT.\n",
      my_progname);
  printf(
      "  %s delete [OPTIONS...] <NAME> [FILES...] delete specified "
      "backup or individual files from the cloud service.\n",
      my_progname);

  puts("\nOptions:");
  my_print_help(my_long_options);
}

static bool get_one_option(int optid,
                           const struct my_option *opt __attribute__((unused)),
                           char *argument) {
  switch (optid) {
    case '?':
      usage();
      exit(0);
    case 'V':
      print_version();
      exit(0);
    case OPT_SWIFT_PASSWORD:
    case OPT_SWIFT_KEY:
    case OPT_SWIFT_TENANT:
    case OPT_SWIFT_TENANT_ID:
    case OPT_S3_ACCESS_KEY:
    case OPT_S3_SECRET_KEY:
    case OPT_S3_SESSION_TOKEN:
    case OPT_GOOGLE_ACCESS_KEY:
    case OPT_GOOGLE_SECRET_KEY:
    case OPT_GOOGLE_SESSION_TOKEN:
    case OPT_AZURE_ACCOUNT:
    case OPT_AZURE_ACCESS_KEY:
      if (argument != nullptr) {
        while (*argument) *argument++ = 0;  // Destroy argument
      }
      break;
    case OPT_HEADER:
      if (argument != nullptr) {
        extra_http_headers.insert(parse_http_header(argument));
      }
      break;
    case OPT_CURL_RETRIABLE_ERRORS:
      if (argument != nullptr) {
        std::istringstream iss(argument);
        for (std::string val; std::getline(iss, val, ',');) {
          char *ptr;
          long int code = strtol(val.c_str(), &ptr, 10);
          if (!*ptr) {
            http_client.set_curl_retriable_errors(
                std::move(static_cast<CURLcode>(code)));
          }
        }
      }
      break;
    case OPT_HTTP_RETRIABLE_ERRORS:
      if (argument != nullptr) {
        std::istringstream iss(argument);
        for (std::string val; std::getline(iss, val, ',');) {
          char *ptr;
          long int code = strtol(val.c_str(), &ptr, 10);
          if (!*ptr) {
            http_client.set_http_retriable_errors(std::move(code));
          }
        }
      }
      break;
  }

  return (false);
}

static const char *load_default_groups[] = {"xbcloud", 0};

static void get_env_args() {
  get_env_value(opt_swift_auth_url, "OS_AUTH_URL");
  get_env_value(opt_swift_tenant, "OS_TENANT_NAME");
  get_env_value(opt_swift_tenant_id, "OS_TENANT_ID");
  get_env_value(opt_swift_user, "OS_USERNAME");
  get_env_value(opt_swift_password, "OS_PASSWORD");
  get_env_value(opt_swift_domain, "OS_USER_DOMAIN");
  get_env_value(opt_swift_domain_id, "OS_USER_DOMAIN_ID");
  get_env_value(opt_swift_project_domain, "OS_PROJECT_DOMAIN");
  get_env_value(opt_swift_project_domain_id, "OS_PROJECT_DOMAIN_ID");
  get_env_value(opt_swift_region, "OS_REGION_NAME");
  get_env_value(opt_swift_storage_url, "OS_STORAGE_URL");
  get_env_value(opt_cacert, "OS_CACERT");

  /* Below block should always be above AWS_* and should not be moved because
  the order of prefrence are like S3_ACCESS_KEY_ID, AWS_ACCESS_KEY_ID and
  ACCESS_KEY  */
  get_env_value(opt_s3_access_key, "S3_ACCESS_KEY_ID");
  get_env_value(opt_s3_secret_key, "S3_SECRET_ACCESS_KEY");
  get_env_value(opt_s3_session_token, "S3_SESSION_TOKEN");
  get_env_value(opt_s3_storage_class, "S3_STORAGE_CLASS");
  get_env_value(opt_s3_region, "S3_DEFAULT_REGION");
  get_env_value(opt_cacert, "S3_CA_BUNDLE");
  get_env_value(opt_s3_endpoint, "S3_ENDPOINT");

  get_env_value(opt_s3_access_key, "AWS_ACCESS_KEY_ID");
  get_env_value(opt_s3_secret_key, "AWS_SECRET_ACCESS_KEY");
  get_env_value(opt_s3_session_token, "AWS_SESSION_TOKEN");
  get_env_value(opt_s3_storage_class, "AWS_STORAGE_CLASS");
  get_env_value(opt_s3_region, "AWS_DEFAULT_REGION");
  get_env_value(opt_cacert, "AWS_CA_BUNDLE");
  get_env_value(opt_s3_endpoint, "AWS_ENDPOINT");

  get_env_value(opt_s3_access_key, "ACCESS_KEY_ID");
  get_env_value(opt_s3_secret_key, "SECRET_ACCESS_KEY");
  get_env_value(opt_s3_region, "DEFAULT_REGION");
  get_env_value(opt_s3_endpoint, "ENDPOINT");

  get_env_value(opt_azure_account, "AZURE_STORAGE_ACCOUNT");
  get_env_value(opt_azure_container, "AZURE_CONTAINER_NAME");
  get_env_value(opt_azure_access_key, "AZURE_ACCESS_KEY");
  get_env_value(opt_azure_storage_class, "AZURE_STORAGE_CLASS");
  get_env_value(opt_azure_endpoint, "AZURE_ENDPOINT");

  get_env_value(opt_google_access_key, "ACCESS_KEY_ID");
  get_env_value(opt_google_secret_key, "SECRET_ACCESS_KEY");
  get_env_value(opt_google_session_token, "SESSION_TOKEN");
  get_env_value(opt_google_region, "DEFAULT_REGION");
  get_env_value(opt_google_endpoint, "ENDPOINT");
}

static char **defaults_argv = nullptr;
static MEM_ROOT argv_alloc{PSI_NOT_INSTRUMENTED, 512};

static bool parse_args(int argc, char **argv) {
  if (load_defaults("my", load_default_groups, &argc, &argv, &argv_alloc)) {
    return true;
  }

  defaults_argv = argv;

  if (handle_options(&argc, &argv, my_long_options, get_one_option)) {
    return true;
  }

  const char *command;

  if (argc < 1) {
    msg_ts("Command isn't specified. Supported commands are put and get\n");
    usage();
    return true;
  }

  command = argv[0];
  argc--;
  argv++;

  get_env_args();

  if (strcasecmp(command, "put") == 0) {
    opt_mode = MODE_PUT;
  } else if (strcasecmp(command, "get") == 0) {
    opt_mode = MODE_GET;
  } else if (strcasecmp(command, "delete") == 0) {
    opt_mode = MODE_DELETE;
  } else if (strcasecmp(command, "probe") == 0) {
    /* probe mode: do the bucket/credential check and exit. No backup
       name required. Wrap your pipeline as:
         xbcloud probe <flags> && xtrabackup ... | xbcloud put <flags> NAME
       so xtrabackup never starts streaming bytes against a broken cloud
       endpoint. */
    opt_mode = MODE_PROBE;
  } else {
    msg_ts("Unknown command %s. Supported: put, get, delete, probe\n",
           command);
    usage();
    return true;
  }

  /* probe mode does not require a backup name; everything else does. */
  if (opt_mode != MODE_PROBE) {
    if (argc < 1) {
      msg_ts("Backup name is required argument\n");
      return true;
    }
    backup_name = argv[0];
    argc--;
    argv++;
  }

  std::string backup_uri = backup_name;
  std::string bucket_name;
  bool backup_name_uri_formatted = false;

  /* parse the backup name */
  if (starts_with(backup_uri, "s3://")) {
    backup_name_uri_formatted = true;
    opt_storage = S3;
    backup_uri = backup_uri.substr(5);
  } else if (starts_with(backup_uri, "google://")) {
    backup_name_uri_formatted = true;
    opt_storage = GOOGLE;
    backup_uri = backup_uri.substr(9);
  } else if (starts_with(backup_uri, "swift://")) {
    backup_name_uri_formatted = true;
    opt_storage = SWIFT;
    backup_uri = backup_uri.substr(8);
  } else if (starts_with(backup_uri, "azure://")) {
    backup_name_uri_formatted = true;
    opt_storage = AZURE;
    backup_uri = backup_uri.substr(8);
  }
  if (backup_name_uri_formatted) {
    auto slash_pos = backup_uri.find('/');
    if (slash_pos != std::string::npos) {
      bucket_name = backup_uri.substr(0, slash_pos);
      backup_name = backup_uri.substr(slash_pos + 1);
      rtrim_slashes(backup_name);
      if (opt_storage == S3) {
        my_free(opt_s3_bucket);
        opt_s3_bucket =
            my_strdup(PSI_NOT_INSTRUMENTED, bucket_name.c_str(), MYF(MY_WME));
      } else if (opt_storage == GOOGLE) {
        my_free(opt_google_bucket);
        opt_google_bucket =
            my_strdup(PSI_NOT_INSTRUMENTED, bucket_name.c_str(), MYF(MY_WME));
      } else if (opt_storage == SWIFT) {
        my_free(opt_swift_container);
        opt_swift_container =
            my_strdup(PSI_NOT_INSTRUMENTED, bucket_name.c_str(), MYF(MY_WME));
      } else if (opt_storage == AZURE) {
        my_free(opt_azure_container);
        opt_azure_container =
            my_strdup(PSI_NOT_INSTRUMENTED, bucket_name.c_str(), MYF(MY_WME));
      }
    }
  }

  if (opt_azure_development_storage) {
    opt_storage = AZURE;
  }

  /* validate arguments */
  if (opt_storage == SWIFT) {
    if (opt_swift_user == nullptr) {
      msg_ts("Swift user is not specified\n");
      return true;
    }
    if (opt_swift_container == nullptr) {
      msg_ts("Swift container is not specified\n");
      return true;
    }
    if (opt_swift_auth_url == nullptr) {
      msg_ts("Swift auth URL is not specified\n");
      return true;
    }
  } else if (opt_storage == AZURE) {
    if (opt_azure_account == nullptr && !opt_azure_development_storage) {
      msg_ts("Azure account name is not specified\n");
      return true;
    }
  }

  while (argc > 0) {
    partial_file_list.insert(*argv);
    --argc;
    ++argv;
  }

  return (0);
}

typedef struct {
  /* used to create FIFO file and print thread number to log */
  uint thread_id;
  std::atomic<bool> *has_errors;
  struct global_list_t *global_list;
  const std::string *container;
  Object_store *store;
} download_thread_ctxt_t;

typedef struct {
  /* used to create FIFO file and print thread number to log */
  uint thread_id;
  std::atomic<bool> *has_errors;
  Http_buffer *buf_md5;
  const std::string *container;
  Object_store *store;
} put_thread_ctxt_t;

struct file_entry_t {
  my_off_t chunk_idx;
  my_off_t offset;
  std::string path;
};

/* PXB-3671 prototype: per-file multipart upload context held in mpfilehash.
   Sequential streaming over the new range-based Multipart_uploader: the
   put_func thread accumulates xbstream frame bytes into part_buf, and
   whenever it crosses part_size it calls uploader->upload_part(next_part_num,
   slice) and increments. On EOF the remainder becomes the final part and
   commit() finalizes. Intra-file parallelism is not used in xbcloud's PUT
   path because frames arrive sequentially from xbstream; the parallel
   per-part model belongs in xtrabackup's Phase 2 ds_cloud (see mpcat for a
   standalone demonstration of that). */
struct mp_upload_t {
  /* Shared Stream_multipart_writer (see multipart.h) drives the
     buffer + flush + commit / small-file fast-path logic. Both
     xbcloud's put_func and xtrabackup's ds_cloud use the same class
     -- single source of truth for the multipart streaming protocol. */
  std::unique_ptr<Stream_multipart_writer> writer;
};

/**
  Build filename with proper chunk index length.

@param [in]    file_name   filename part of the chunk
@param [in]    idx         chunk index

@return string containing the file name */
std::string build_file_name(const std::string &file_name, my_off_t idx) {
  std::stringstream file_name_s;
  file_name_s << file_name << "." << std::setw(chunk_index_prefix_len)
              << std::setfill('0') << idx;
  return file_name_s.str();
}

/**
  Check if file should be skipped due to user specified list of files.
  This is used on Download and Delete

@param [in]    file_name   filename to check
@param [in]    backup_name backup name

@return true in case we should skip this file */

static bool skip_file(const std::string &file_name,
                      const std::string &backup_name) {
  if (!partial_file_list.empty() &&
      partial_file_list.count(file_name.substr(backup_name.length() + 1)) < 1)
    return true;

  return false;
}

void put_func(put_thread_ctxt_t &cntx) {
  std::thread ev;
  Event_handler h(opt_parallel > 0 ? opt_parallel : 1);
  std::unordered_map<std::string, std::unique_ptr<file_entry_t>> filehash;
  /* PXB-3671 prototype: parallel map of per-file Multipart_uploader. Used
     only when opt_multipart_upload is true. Keyed by the source file path
     the same way filehash is. */
  std::unordered_map<std::string, std::unique_ptr<mp_upload_t>> mpfilehash;
  xb_rstream_t *stream;
  if (opt_threads > 1) {
    char filename[FN_REFLEN];
    snprintf(filename, sizeof(filename), "%s%s%lu", opt_fifo_dir, "/thread_",
             (ulong)cntx.thread_id);
    stream = xb_stream_read_new_fifo(filename, opt_fifo_timeout);
    if (stream == nullptr) {
      msg_ts(
          "%s: xb_stream_read_new_fifo() failed for thread %d. Possibly sender "
          "did "
          "not start.\n",
          my_progname, cntx.thread_id);
      cntx.has_errors->store(true);
      goto end;
    }

  } else {
    stream = xb_stream_read_new_stdin();
    if (stream == nullptr) {
      msg_ts("%s: xb_stream_read_new_stdin() failed.\n", my_progname);
      cntx.has_errors->store(true);
      goto end;
    }
  }

  xb_rstream_chunk_t chunk;
  xb_rstream_result_t res;

  memset(&chunk, 0, sizeof(chunk));

  if (!h.init()) {
    msg_ts("%s: Failed to initialize event handler.\n", my_progname);
    goto end;
  }
  h.install_rate_logger(static_cast<double>(opt_rate_log_interval));
  ev = h.run();

  do {
    res = xb_stream_read_chunk(stream, &chunk);
    if (res != XB_STREAM_READ_CHUNK) {
      my_free(chunk.raw_data);
      my_free(chunk.sparse_map);
      break;
    }
    if (chunk.type == XB_CHUNK_TYPE_UNKNOWN &&
        !(chunk.flags & XB_STREAM_FLAG_IGNORABLE)) {
      continue;
    }

    file_entry_t *entry = filehash[chunk.path].get();
    if (entry == nullptr) {
      entry = (filehash[chunk.path] = make_unique<file_entry_t>()).get();
      entry->path = chunk.path;
    }

    if (chunk.type == XB_CHUNK_TYPE_PAYLOAD) {
      res = (xb_rstream_result_t)xb_stream_validate_checksum(&chunk);
      if (res != XB_STREAM_READ_CHUNK) {
        break;
      }

      if (entry->offset != chunk.offset) {
        msg_ts(
            "%s: out-of-order chunk: real offset = 0x%llx, "
            "expected offset = 0x%llx\n",
            my_progname, chunk.offset, entry->offset);
        res = XB_STREAM_READ_ERROR;
        break;
      }
    }

    if (opt_multipart_upload) {
      /* PXB-3671 prototype: one-file-per-object via backend multipart upload.
         The new range-based Multipart_uploader API is driven sequentially
         here because xbstream frames arrive in file order; intra-file
         parallel uploads are a Phase 2 / xtrabackup-side capability and are
         exercised separately by mpcat. */
      std::string object_name = backup_name;
      object_name.append("/").append(chunk.path);

      mp_upload_t *mp = mpfilehash[chunk.path].get();
      if (mp == nullptr) {
        auto entry_ptr = std::make_unique<mp_upload_t>();
        entry_ptr->writer = std::make_unique<Stream_multipart_writer>(
            cntx.store, *cntx.container, object_name, &h,
            opt_multipart_memory_budget,
            static_cast<size_t>(opt_multipart_threshold),
            static_cast<size_t>(opt_multipart_part_size));
        /* xbcloud (unlike ds_cloud) prefers an async small-file PUT so the
           producer thread doesn't block on each tiny file when ingesting
           many of them. Wire the writer's small-file path through the
           Event_handler-backed async_upload_object. */
        auto *errp = cntx.has_errors;
        int thread_id = cntx.thread_id;
        auto *container = cntx.container;
        Object_store *store = cntx.store;
        Event_handler *event_handler = &h;
        entry_ptr->writer->set_async_small_file_uploader(
            [errp, thread_id, container, store, event_handler](
                const std::string &name, const Http_buffer &body) -> bool {
              size_t length = body.size();
              return store->async_upload_object(
                  *container, name, body, event_handler,
                  [errp, thread_id, name, length](bool ok,
                                                  const Http_buffer &) {
                    if (ok) {
                      msg_ts(
                          "%s: [%d] small-file PUT done: %s, size: %zu\n",
                          my_progname, thread_id, name.c_str(), length);
                    } else {
                      msg_ts(
                          "%s: [%d] error: small-file PUT failed: %s, "
                          "size: %zu\n",
                          my_progname, thread_id, name.c_str(), length);
                      errp->store(true);
                    }
                  });
            });
        mp = (mpfilehash[chunk.path] = std::move(entry_ptr)).get();
      }

      /* SPARSE frames require offset-honoring writes / hole reconstruction
         which is not supported in this phase; folds into the unified
         backup_meta.json manifest (PXB-3754). */
      if (chunk.type == XB_CHUNK_TYPE_SPARSE) {
        msg_ts(
            "%s: [%d] error: sparse file %s is not supported by xbcloud's "
            "multipart upload. Use --multipart-upload=OFF for sparse-file "
            "backups.\n",
            my_progname, cntx.thread_id, chunk.path);
        cntx.has_errors->store(true);
        my_free(chunk.raw_data);
        my_free(chunk.sparse_map);
        memset(&chunk, 0, sizeof(chunk));
        continue;
      }
      if (chunk.type == XB_CHUNK_TYPE_PAYLOAD && chunk.length > 0) {
        if (!mp->writer->append(static_cast<const char *>(chunk.data),
                                chunk.length)) {
          msg_ts("%s: [%d] writer->append failed for %s\n", my_progname,
                 cntx.thread_id, object_name.c_str());
          cntx.has_errors->store(true);
        }
      }

      /* Rollover not yet supported in streaming mode (size is unknown up
         front). If the writer's running bytes_appended crosses the
         configured per-object cap, abort with a clear message. */
      if (mp->writer->bytes_appended() > opt_multipart_rollover_threshold) {
        msg_ts(
            "%s: [%d] error: streaming file %s exceeded "
            "--multipart-rollover-threshold (%llu bytes). Use "
            "--multipart-from-file for files larger than the threshold.\n",
            my_progname, cntx.thread_id, chunk.path,
            opt_multipart_rollover_threshold);
        cntx.has_errors->store(true);
        my_free(chunk.raw_data);
        my_free(chunk.sparse_map);
        memset(&chunk, 0, sizeof(chunk));
        continue;
      }

      /* Update file_entry_t bookkeeping BEFORE any erase below. */
      entry->offset += chunk.length;
      entry->chunk_idx++;

      /* EOF: writer->close() finalizes (multipart commit OR small-file
         single PUT depending on whether parts were already submitted). */
      if (chunk.type == XB_CHUNK_TYPE_EOF) {
        if (!cntx.has_errors->load()) {
          if (!mp->writer->close()) {
            msg_ts("%s: [%d] writer->close failed for %s\n", my_progname,
                   cntx.thread_id, object_name.c_str());
            cntx.has_errors->store(true);
          } else if (!mp->writer->small_file_path_taken()) {
            msg_ts("%s: [%d] multipart commit done for %s\n", my_progname,
                   cntx.thread_id, object_name.c_str());
          }
        }
        mpfilehash.erase(chunk.path);
        filehash.erase(chunk.path);
      }

      my_free(chunk.raw_data);
      my_free(chunk.sparse_map);
      memset(&chunk, 0, sizeof(chunk));
      continue;
    }

    /* Legacy chunk-per-PUT path follows. */
    std::string file_name = build_file_name(chunk.path, entry->chunk_idx);
    std::string object_name = backup_name;
    object_name.append("/").append(file_name);

    Http_buffer buf = Http_buffer();
    buf.assign_buffer(static_cast<char *>(chunk.raw_data), chunk.buflen,
                      chunk.raw_length);

    if (opt_md5) {
      cntx.buf_md5->append(hex_encode(buf.md5()));
      cntx.buf_md5->append("  ");
      cntx.buf_md5->append(file_name);
      cntx.buf_md5->append("\n");
    }

    cntx.store->async_upload_object(
        *cntx.container, object_name, buf, &h,
        std::bind(
            [&](bool ok, std::string path, size_t length,
                std::atomic<bool> *err) {
              if (ok) {
                msg_ts("%s: [%d] successfully uploaded chunk: %s, size: %zu\n",
                       my_progname, cntx.thread_id, path.c_str(), length);
              } else {
                msg_ts(
                    "%s: [%d] error: failed to upload chunk: %s, size: %zu\n",
                    my_progname, cntx.thread_id, path.c_str(), length);
                err->store(true);
              }
            },
            std::placeholders::_1, object_name, chunk.raw_length,
            cntx.has_errors));

    entry->offset += chunk.length;
    entry->chunk_idx++;

    if (chunk.type == XB_CHUNK_TYPE_EOF) {
      filehash.erase(entry->path);
    }

    /* Reset chunk */
    memset(&chunk, 0, sizeof(chunk));
  } while (!cntx.has_errors->load());

  /* Abort any still-open writers on early exit. */
  for (auto &kv : mpfilehash) {
    msg_ts("%s: [%d] aborting in-flight multipart on early exit: %s\n",
           my_progname, cntx.thread_id, kv.first.c_str());
    kv.second->writer->abort();
  }
  mpfilehash.clear();

  h.stop();
  ev.join();

end:
  if (stream != nullptr) xb_stream_read_done(stream);
}

/* Upload `segment_size` bytes from the current fd position to
   `object_name` via one Multipart_uploader. Returns true on success.
   On entry, fd is positioned at the start of this segment. On exit,
   fd has advanced by segment_size. */
static bool upload_one_segment_from_fd(Object_store *store, Event_handler *h,
                                       const std::string &container,
                                       const std::string &object_name, int fd,
                                       uint64_t segment_size) {
  Object_store_multipart_helper helper(store, container, object_name);
  Multipart_uploader uploader(&helper, h, opt_multipart_memory_budget);

  if (!uploader.start()) {
    msg_ts("%s: multipart start failed for %s\n", my_progname,
           object_name.c_str());
    return false;
  }

  /* Buffer sized to the schedule's maximum tier (600 MiB at >= 1 TiB).
     A single allocation is reused across parts. dynamic_part_size never
     returns more than 1 GiB, but the ramp doesn't cross that within a
     <= 5 TiB segment because the >= 1 TiB tier plateaus at 600 MiB. */
  constexpr size_t MAX_PART_BUF = 1024ULL * 1024ULL * 1024ULL;
  std::unique_ptr<char[]> buf(new char[MAX_PART_BUF]);

  int part_number = 1;
  uint64_t segment_read = 0;
  while (segment_read < segment_size) {
    size_t part_size =
        opt_multipart_part_size != 0
            ? static_cast<size_t>(opt_multipart_part_size)
            : dynamic_part_size(segment_read);
    if (part_size > MAX_PART_BUF) part_size = MAX_PART_BUF;
    size_t to_read =
        static_cast<size_t>(std::min<uint64_t>(part_size,
                                               segment_size - segment_read));
    ssize_t n = read(fd, buf.get(), to_read);
    if (n <= 0) {
      msg_ts("%s: read error at segment-offset %lu: %s\n", my_progname,
             segment_read, strerror(errno));
      uploader.abort();
      return false;
    }
    if (!uploader.upload_part(part_number, buf.get(),
                              static_cast<size_t>(n))) {
      msg_ts("%s: upload_part %d failed for %s\n", my_progname, part_number,
             object_name.c_str());
      return false;
    }
    part_number++;
    segment_read += static_cast<uint64_t>(n);
  }

  if (segment_read != segment_size) {
    msg_ts("%s: short read in segment %s: %lu of %lu bytes\n", my_progname,
           object_name.c_str(), segment_read, segment_size);
    uploader.abort();
    return false;
  }

  if (!uploader.commit()) {
    msg_ts("%s: multipart commit failed for %s\n", my_progname,
           object_name.c_str());
    return false;
  }
  return true;
}

/* PXB-3671 prototype: upload one local file directly via the new async
   Multipart_uploader, applying the tiered dynamic part-size schedule.
   Bypasses xbstream and the put_func pipe-reading loop entirely; this
   is the path xtrabackup's Phase 2 ds_cloud will use, and it doubles
   as the test harness for the multipart machinery against real (large)
   file data.

   Rollover: when file_size exceeds --multipart-rollover-threshold the
   file is split into <name>.part-001, <name>.part-002, ... segments
   each <= threshold, and a <name>.manifest.json sidecar lists them.
   The default threshold equals S3's 5 TiB single-object hard cap, so
   rollover only kicks in for genuinely-oversize files unless the knob
   is lowered for testing. */
bool xbcloud_put_from_file(Object_store *store, const std::string &container,
                           const std::string &backup_name,
                           const std::string &local_path) {
  /* Container check / create, matching xbcloud_put's behavior. */
  bool exists;
  if (!store->container_exists(container, exists)) return false;
  if (!exists && !store->create_container(container)) return false;

  /* stat() the local file to learn its size up front. */
  struct stat st;
  if (stat(local_path.c_str(), &st) != 0) {
    msg_ts("%s: cannot stat local file %s: %s\n", my_progname,
           local_path.c_str(), strerror(errno));
    return false;
  }
  uint64_t file_size = static_cast<uint64_t>(st.st_size);

  std::string base = local_path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  std::string object_name = backup_name + "/" + base;

  const uint64_t rollover = opt_multipart_rollover_threshold;
  const bool rolled_over = file_size > rollover;
  const uint64_t n_segments =
      rolled_over ? (file_size + rollover - 1) / rollover : 1;

  msg_ts(
      "%s: multipart-from-file: %s (%lu bytes) -> %s/%s, "
      "rollover_threshold=%lu MiB, segments=%lu, memory_budget=%llu MiB\n",
      my_progname, local_path.c_str(), file_size, container.c_str(),
      object_name.c_str(), rollover / (1024 * 1024), n_segments,
      opt_multipart_memory_budget / (1024 * 1024));

  int fd = open(local_path.c_str(), O_RDONLY);
  if (fd < 0) {
    msg_ts("%s: cannot open local file %s: %s\n", my_progname,
           local_path.c_str(), strerror(errno));
    return false;
  }

  Event_handler h(opt_parallel > 0 ? opt_parallel : 1);
  if (!h.init()) {
    msg_ts("%s: Failed to initialize event handler.\n", my_progname);
    close(fd);
    return false;
  }
  h.install_rate_logger(static_cast<double>(opt_rate_log_interval));
  std::thread ev = h.run();

  bool ok = true;
  std::vector<rollover_segment_t> segments;
  uint64_t total_read = 0;

  for (uint64_t seg = 0; seg < n_segments && ok; ++seg) {
    uint64_t segment_size =
        std::min<uint64_t>(rollover, file_size - total_read);
    std::string seg_name;
    if (rolled_over) {
      char suffix[32];
      snprintf(suffix, sizeof(suffix), ".part-%03lu", seg + 1);
      seg_name = object_name + suffix;
    } else {
      seg_name = object_name;
    }
    msg_ts("%s: segment %lu/%lu -> %s (%lu bytes)\n", my_progname, seg + 1,
           n_segments, seg_name.c_str(), segment_size);
    if (!upload_one_segment_from_fd(store, &h, container, seg_name, fd,
                                    segment_size)) {
      ok = false;
      break;
    }
    segments.push_back({seg_name, segment_size});
    total_read += segment_size;
  }

  if (ok && rolled_over) {
    std::string manifest_key = object_name + ".manifest.json";
    std::string manifest_body =
        build_rollover_manifest(base, file_size, rollover, segments);
    Http_buffer body;
    body.append(manifest_body.data(), manifest_body.size());
    if (!store->upload_object(container, manifest_key, body)) {
      msg_ts("%s: failed to upload rollover manifest %s\n", my_progname,
             manifest_key.c_str());
      ok = false;
    } else {
      msg_ts("%s: rollover manifest written: %s (%zu bytes, %zu segments)\n",
             my_progname, manifest_key.c_str(), manifest_body.size(),
             segments.size());
    }
  }

  if (ok) {
    msg_ts("%s: multipart-from-file done: %s -> %s/%s (%lu bytes total)\n",
           my_progname, local_path.c_str(), container.c_str(),
           object_name.c_str(), total_read);
  }

  close(fd);
  h.stop();
  ev.join();
  return ok;
}

bool xbcloud_put(Object_store *store, const std::string &container,
                 const std::string &backup_name) {
  bool exists;
  std::atomic<bool> has_errors{false};
  Http_buffer buf_md5 = Http_buffer();
  std::string last_file_prefix = backup_name + "/xtrabackup_tablespaces";
  auto last_file_size = last_file_prefix.size();
  bool file_found = false;
  if (!store->container_exists(container, exists)) {
    return false;
  }

  if (!exists) {
    if (!store->create_container(container)) {
      return false;
    }
  }

  std::vector<std::string> object_list;
  if (!store->list_objects_in_directory(container, backup_name, object_list)) {
    return false;
  }

  if (!object_list.empty()) {
    msg_ts("%s: error: backup named %s already exists!\n", my_progname,
           backup_name.c_str());
    return false;
  }

  /* Create data copying threads */
  put_thread_ctxt_t *data_threads = (put_thread_ctxt_t *)my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(put_thread_ctxt_t) * (opt_threads + 1),
      MYF(MY_FAE));
  std::vector<std::thread> threads;
  for (uint i = 0; i < (uint)opt_threads; i++) {
    data_threads[i].thread_id = i;
    data_threads[i].has_errors = &has_errors;
    data_threads[i].store = store;
    data_threads[i].container = &container;
    data_threads[i].buf_md5 = new Http_buffer();

    threads.push_back(std::thread(put_func, std::ref(data_threads[i])));
  }

  for (uint i = 0; i < (uint)opt_threads; i++) {
    threads.at(i).join();
    if (!has_errors.load() && opt_md5) {
      buf_md5.append(*data_threads[i].buf_md5);
    }
  }

  if (!has_errors.load() && opt_md5) {
    msg_ts("%s: Uploading md5\n", my_progname);
    if (!store->upload_object(container, backup_name + ".md5", buf_md5)) {
      msg_ts("%s: Upload failed: Error uploading md5.\n", my_progname);
      has_errors.store(true);
      goto cleanup;
    }
  }

  if (has_errors.load() ||
      !store->list_objects_in_directory(container, backup_name, object_list) ||
      object_list.size() == 0) {
    msg_ts("%s: Upload failed.\n", my_progname);
    has_errors.store(true);
    goto cleanup;
  }

  /* check if the last_file (xtrabackup_tablespaces.gz.00000000000 or
  xtrabackup_tablespaces.00000000000000000000) is uploaded to cloud storage to
  determine successful xbcloud "put" operation. */
  for (auto cur_file = object_list.rbegin(); cur_file != object_list.rend();
       cur_file++) {
    if (cur_file->size() >= last_file_size &&
        cur_file->substr(0, last_file_size).compare(last_file_prefix) == 0) {
      file_found = true;
      break;
    }
  }
  if (!file_found) {
    msg_ts(
        "%s: Upload failed: backup is incomplete.\nBackup doesn't contain "
        "last file with prefix xtrabackup_tablespaces in the cloud storage\n",
        my_progname);
    has_errors.store(true);
    goto cleanup;
  }

  msg_ts("%s: Upload completed.\n", my_progname);

cleanup:
  for (uint i = 0; i < (uint)opt_threads; i++) {
    delete (data_threads[i].buf_md5);
    char filename[FN_REFLEN];
    snprintf(filename, sizeof(filename), "%s%s%lu", opt_fifo_dir, "/thread_",
             (ulong)i);
    unlink(filename);
  }

  my_free(data_threads);
  return !has_errors.load();
}

/** Validates a chunk name splitting the name and index part of it.
@param[in]        chunk_name  string holding the chunk name
@param[in,out]    file_name   filename part of the chunk
@param[in,out]    idx         chunk index
@return true in case of success or false otherwise */
bool chunk_name_to_file_name(const std::string &chunk_name,
                             std::string &file_name, my_off_t &idx) {
  if (chunk_name.size() < 22 && chunk_name[chunk_name.size() - 21] != '.') {
    /* chunk name is invalid */
    return false;
  }
  file_name = chunk_name.substr(0, chunk_name.size() - 21);
  idx = atoll(&chunk_name.c_str()[chunk_name.size() - 20]);
  return true;
}

/**
 * Delete a backup directory from object storage.
 *
 * @param store Object store implementation.
 * @param container Container/bucket name.
 * @param backup_name Backup directory to delete.
 * @return true on success, false on error.
 */
bool xbcloud_delete(Object_store *store, const std::string &container,
                    const std::string &backup_name) {
  std::vector<std::string> files;
  std::vector<std::string> dirs;

  // First pass lists files/dirs to validate existence and drive deletion.
  if (!store->list_objects_files_and_dirs(container, backup_name, files,
                                          dirs)) {
    msg_ts("%s: Delete failed. Cannot list %s.\n", my_progname,
           backup_name.c_str());
    return false;
  }
  if (files.empty() && dirs.empty()) {
    msg_ts("%s: error: backup named %s doesn't exists!\n", my_progname,
           backup_name.c_str());
    return false;
  }

  Event_handler h(opt_parallel > 0 ? opt_parallel : 1);
  if (!h.init()) {
    msg_ts("%s: Failed to initialize event handler.\n", my_progname);
    return false;
  }
  auto thread = h.run();

  bool error = false;
  for (const auto &obj : files) {
    std::string file_name;
    my_off_t idx;
    if (error) break;
    if (!chunk_name_to_file_name(obj, file_name, idx)) {
      continue;
    }
    if (skip_file(file_name, backup_name)) {
      continue;
    }
    msg_ts("%s: Deleting %s.\n", my_progname, obj.c_str());
    if (!store->async_delete_object(
            container, obj, &h,
            std::bind(
                [](bool success, std::string obj, bool *error) {
                  if (!success) {
                    msg_ts("%s: Delete failed. Cannot delete %s.\n",
                           my_progname, obj.c_str());
                    *error = true;
                  }
                },
                std::placeholders::_1, obj, &error))) {
      h.stop();
      thread.join();
      return false;
    }
  }

  h.stop();
  thread.join();

  if (error) {
    msg_ts("%s: Delete failed.\n", my_progname);
    return false;
  }

  if (!dirs.empty()) {
    std::sort(dirs.begin(), dirs.end(), std::greater<std::string>());
    for (const auto &d : dirs) {
      msg_ts("%s: Deleting directory %s.\n", my_progname, d.c_str());
      if (!store->delete_object(container, d)) {
        msg_ts("%s: Delete failed. Cannot delete directory %s.\n", my_progname,
               d.c_str());
        return false;
      }
    }

    // Delete the root directory of the backup
    msg_ts("%s: Deleting directory %s.\n", my_progname, backup_name.c_str());
    if (!store->delete_object(container, backup_name)) {
      msg_ts("%s: Warning: Failed to delete root directory %s.\n", my_progname,
             backup_name.c_str());
    }
  }

  msg_ts("%s: Delete completed.\n", my_progname);

  return true;
}

void download_func(download_thread_ctxt_t &cntx) {
  auto thread_id = cntx.thread_id;
  File fd;
  std::thread ev;
  std::atomic<bool> *error = cntx.has_errors;
  thread_state_t *thread_state = new struct thread_state_t;
  Event_handler h(opt_parallel > 0 ? opt_parallel : 1);

  if (opt_threads > 1) {
    char fifo_filename[FN_REFLEN];
    snprintf(fifo_filename, sizeof(fifo_filename), "%s%s%lu", opt_fifo_dir,
             "/thread_", (ulong)thread_id);
    fd = open_fifo_for_write_with_timeout(fifo_filename, opt_fifo_timeout);
    if (fd < 0) {
      msg_ts("%s: [%d] my_open failed for file: %s.\n", my_progname, thread_id,
             fifo_filename);
      error->store(true);
      goto end;
    }
  } else {
    fd = fileno(stdout);
  }
  if (!h.init()) {
    msg_ts("%s: Failed to initialize event handler.\n", my_progname);
    error->store(true);
    goto end;
  }
  ev = h.run();

  while (true) {
    /* Do not queue more than what we can handle. If another thread has free
     * workable slots let it queue the file. */
    if (thread_state->in_progress_files_size() >= opt_parallel) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (cntx.global_list->empty() && thread_state->file_list_empty()) {
      break;
    }
    /* We ensure we don't have any pending callback to be processed */
    if (error->load() && !thread_state->in_progress_files_empty()) {
      continue;
    }
    if (error->load()) break;

    /* Check if we have any file available from thread list or global list */
    file_metadata_t file;
    if (!thread_state->next_file(file) && !cntx.global_list->next_file(file)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (!thread_state->start_file(file)) {
      /* This file is already been downloaded. skipping */
      continue;
    }
    my_off_t id = thread_state->next_chunk(file);
    std::string chunk = build_file_name(file.name, id);

    msg_ts("%s: [%d] Downloading %s.\n", my_progname, thread_id, chunk.c_str());
    cntx.store->async_download_object(
        *cntx.container, chunk, &h,
        std::bind(
            [&thread_state](bool success, const Http_buffer &contents,
                            std::string chunk, my_off_t idx,
                            std::atomic<bool> *error, uint thread_id, File fd,
                            file_metadata_t file) {
              if (!success) {
                error->store(true);
                msg_ts("%s: [%d] Download failed. Cannot download %s.\n",
                       my_progname, thread_id, chunk.c_str());

              } else if (!my_write(fd,
                                   reinterpret_cast<unsigned char *>(
                                       const_cast<char *>(&contents[0])),
                                   contents.size(), MYF(MY_WME | MY_NABP))) {
                msg_ts("%s: [%d] Download successfull %s, size %zu\n",
                       my_progname, thread_id, chunk.c_str(), contents.size());
              } else {
                msg_ts(
                    "%s: [%d] Download of file %s failed. Cannot write to "
                    "output "
                    "\n",
                    my_progname, thread_id, chunk.c_str());
                error->store(true);
              }
              thread_state->complete_chunk(file, idx);
            },
            std::placeholders::_1, std::placeholders::_2, chunk, id,
            cntx.has_errors, thread_id, fd, file));
  }

  h.stop();
  ev.join();

end:
  delete (thread_state);
  if (fd > 2) {
    my_close(fd, MYF(0));
    fd = -1;
  }
}

bool xbcloud_download(Object_store *store, const std::string &container,
                      const std::string &backup_name) {
  std::vector<std::string> object_list;
  std::atomic<bool> has_errors{false};
  char fullpath[FN_REFLEN];
  if (!store->list_objects_in_directory(container, backup_name, object_list) ||
      object_list.size() == 0) {
    msg_ts("%s: Download failed. Cannot list %s.\n", my_progname,
           backup_name.c_str());
    return false;
  }
  struct global_list_t *global_list = new struct global_list_t;
  for (const auto &obj : object_list) {
    my_off_t idx;
    std::string file_name;
    if (!chunk_name_to_file_name(obj, file_name, idx)) {
      continue;
    }
    if (skip_file(file_name, backup_name)) {
      continue;
    }
    global_list->add(file_name, idx);
  }

  /* Create FIFO files if necessary */

  if (opt_threads > 1) {
    if (my_mkdir(opt_fifo_dir, 0777, MYF(0)) < 0 && my_errno() != EEXIST &&
        my_errno() != EISDIR) {
      char errbuf[MYSYS_STRERROR_SIZE];
      msg_ts("%s: Error creating dir: %s. (%d) %s\n", my_progname, opt_fifo_dir,
             my_errno(), my_strerror(errbuf, sizeof(errbuf), my_errno()));
      return false;
    }
    for (uint i = 0; i < opt_threads; i++) {
      std::string path = "thread_" + std::to_string(i);
      fn_format(fullpath, path.c_str(), opt_fifo_dir, "",
                MYF(MY_RELATIVE_PATH));
      mkfifo(fullpath, 0600);
    }
    msg_ts(
        "Created %ld Named Pipes(FIFO). Waiting up to %ld seconds for xbstream "
        "to open the files for reading.\n",
        opt_threads, opt_fifo_timeout);
  }
  /* Create data copying threads */
  download_thread_ctxt_t *data_threads = (download_thread_ctxt_t *)my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(download_thread_ctxt_t) * (opt_threads + 1),
      MYF(MY_FAE));
  std::vector<std::thread> threads;
  for (uint i = 0; i < (uint)opt_threads; i++) {
    data_threads[i].thread_id = i;
    data_threads[i].has_errors = &has_errors;
    data_threads[i].global_list = global_list;
    data_threads[i].store = store;
    data_threads[i].container = &container;
    threads.push_back(std::thread(download_func, std::ref(data_threads[i])));
  }

  for (uint i = 0; i < (uint)opt_threads; i++) {
    threads.at(i).join();
  }

  if (has_errors.load()) {
    msg_ts("%s: Download failed.\n", my_progname);
  } else {
    msg_ts("%s: Download completed.\n", my_progname);
  }

  delete (global_list);
  my_free(data_threads);

  return !has_errors.load();
}

struct main_exit_hook {
  ~main_exit_hook() {
    http_cleanup();
    my_end(0);
  }
};

int main(int argc, char **argv) {
  MY_INIT(argv[0]);

#ifndef NO_SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif
  xb_libgcrypt_init();

  http_init();
  crc_init();

  /* trick to automatically release some globally alocated resources */
  main_exit_hook exit_hook;

  if (parse_args(argc, argv)) {
    return EXIT_FAILURE;
  }

  check_library_versions();

  std::unique_ptr<Object_store> object_store = nullptr;
  if (opt_verbose) {
    http_client.set_verbose(true);
  }
  if (opt_insecure) {
    http_client.set_insecure(true);
  }
  if (opt_cacert != nullptr) {
    http_client.set_cacaert(opt_cacert);
  }
  if (opt_timeout > 0) {
    http_client.set_timeout(opt_timeout);
  }
  http_client.set_max_retries(opt_max_retries);
  http_client.set_max_backoff(opt_max_backoff);
  if (opt_http_timing) {
    http_timing::enable();
    msg_ts("%s: HTTP timing instrumentation enabled\n", my_progname);
  }

  std::string container_name;

  if (opt_storage == SWIFT) {
    std::string auth_url = opt_swift_auth_url;
    if (!ends_with(auth_url, "/")) {
      auth_url.append("/");
    }

    const char *valid_versions[] = {"/v1/",   "/v2/",   "/v3/",
                                    "/v1.0/", "/v2.0/", "/v3.0/"};
    bool versioned_url = false;
    for (auto version : valid_versions) {
      if (ends_with(opt_swift_auth_url, version)) {
        my_free(opt_swift_auth_version);
        opt_swift_auth_version =
            my_strdup(PSI_NOT_INSTRUMENTED,
                      std::string(version + 2, strlen(version + 2) - 1).c_str(),
                      MYF(MY_FAE));
        versioned_url = true;
      }
    }
    if (!versioned_url) {
      if (opt_swift_auth_version != nullptr) {
        auth_url.append("v");
        auth_url.append(opt_swift_auth_version);
        auth_url.append("/");
      } else {
        opt_swift_auth_version =
            my_strdup(PSI_NOT_INSTRUMENTED, "1.0", MYF(MY_FAE));
        auth_url.append("v");
        auth_url.append(opt_swift_auth_version);
        auth_url.append("/");
      }
    }

    Keystone_client keystone_client(&http_client, auth_url);

    if (opt_swift_key != nullptr) {
      keystone_client.set_key(opt_swift_key);
    }
    if (opt_swift_user != nullptr) {
      keystone_client.set_user(opt_swift_user);
    }
    if (opt_swift_password != nullptr) {
      keystone_client.set_password(opt_swift_password);
    }
    if (opt_swift_tenant != nullptr) {
      keystone_client.set_tenant(opt_swift_tenant);
    }
    if (opt_swift_tenant_id != nullptr) {
      keystone_client.set_tenant_id(opt_swift_tenant_id);
    }
    if (opt_swift_domain != nullptr) {
      keystone_client.set_domain(opt_swift_domain);
    }
    if (opt_swift_domain_id != nullptr) {
      keystone_client.set_domain_id(opt_swift_domain_id);
    }
    if (opt_swift_project_domain != nullptr) {
      keystone_client.set_project_domain(opt_swift_project_domain);
    }
    if (opt_swift_project_domain_id != nullptr) {
      keystone_client.set_project_domain_id(opt_swift_project_domain_id);
    }
    if (opt_swift_project != nullptr) {
      keystone_client.set_project(opt_swift_project);
    }
    if (opt_swift_project_id != nullptr) {
      keystone_client.set_project_id(opt_swift_project_id);
    }

    Keystone_client::auth_info_t auth_info;

    if (opt_swift_auth_version == NULL || *opt_swift_auth_version == '1') {
      /* TempAuth */
      if (!keystone_client.temp_auth(auth_info)) {
        return EXIT_FAILURE;
      }
    } else if (*opt_swift_auth_version == '2') {
      /* Keystone v2 */
      if (!keystone_client.auth_v2(
              opt_swift_region != nullptr ? opt_swift_region : "", auth_info)) {
        return EXIT_FAILURE;
      }
    } else if (*opt_swift_auth_version == '3') {
      /* Keystone v3 */
      if (!keystone_client.auth_v3(
              opt_swift_region != nullptr ? opt_swift_region : "", auth_info)) {
        return EXIT_FAILURE;
      }
    }

    if (opt_swift_storage_url != nullptr) {
      /* override storage url */
      auth_info.url = opt_swift_storage_url;
    }

    msg_ts("Object store URL: %s\n", auth_info.url.c_str());

    object_store = std::unique_ptr<Object_store>(
        new Swift_object_store(&http_client, auth_info.url, auth_info.token,
                               opt_max_retries, opt_max_backoff));

    container_name = opt_swift_container;

  } else if (opt_storage == S3) {
    std::shared_ptr<S3_ec2_instance> ec2_instance =
        std::make_shared<S3_ec2_instance>(&http_client);
    if (opt_s3_access_key == nullptr && opt_s3_secret_key == nullptr &&
        opt_s3_session_token == nullptr) {
      if (ec2_instance->fetch_metadata() &&
          ec2_instance->get_is_ec2_instance_with_profile()) {
        opt_s3_access_key =
            my_strdup(PSI_NOT_INSTRUMENTED,
                      ec2_instance->get_access_key().c_str(), MYF(MY_FAE));
        opt_s3_secret_key =
            my_strdup(PSI_NOT_INSTRUMENTED,
                      ec2_instance->get_secret_key().c_str(), MYF(MY_FAE));
        opt_s3_session_token =
            my_strdup(PSI_NOT_INSTRUMENTED,
                      ec2_instance->get_session_token().c_str(), MYF(MY_FAE));
        msg_ts("%s: Using instance metadata for access and secret key\n",
               my_progname);
      }
    }
    if (opt_s3_access_key == nullptr) {
      msg_ts("S3 access key is not specified\n");
      return EXIT_FAILURE;
    }
    if (opt_s3_secret_key == nullptr) {
      msg_ts("S3 secret key is not specified\n");
      return EXIT_FAILURE;
    }
    std::string region =
        opt_s3_region != nullptr ? opt_s3_region : default_s3_region;
    std::string access_key =
        opt_s3_access_key != nullptr ? opt_s3_access_key : "";
    std::string secret_key =
        opt_s3_secret_key != nullptr ? opt_s3_secret_key : "";
    std::string session_token =
        opt_s3_session_token != nullptr ? opt_s3_session_token : "";
    std::string storage_class =
        opt_s3_storage_class != nullptr ? opt_s3_storage_class : "";
    object_store = std::unique_ptr<Object_store>(new S3_object_store(
        &http_client, region, access_key, secret_key, session_token,
        storage_class, opt_max_retries, opt_max_backoff,
        opt_s3_endpoint != nullptr ? opt_s3_endpoint : "",
        static_cast<s3_bucket_lookup_t>(opt_s3_bucket_lookup),
        static_cast<s3_api_version_t>(opt_s3_api_version)));

    if (opt_s3_bucket == nullptr) {
      msg_ts("%s: S3 bucket is not specified.\n", my_progname);
      return EXIT_FAILURE;
    }

    reinterpret_cast<S3_object_store *>(object_store.get())
        ->set_extra_http_headers(extra_http_headers);

    container_name = opt_s3_bucket;

    if (!reinterpret_cast<S3_object_store *>(object_store.get())
             ->probe_api_version_and_lookup(container_name)) {
      return EXIT_FAILURE;
    }
    if (ec2_instance->get_is_ec2_instance_with_profile()) {
      reinterpret_cast<S3_object_store *>(object_store.get())
          ->set_ec2_instance(ec2_instance);
    }
  } else if (opt_storage == GOOGLE) {
    std::string region =
        opt_google_region != nullptr ? opt_google_region : "us-central-1";
    std::string access_key =
        opt_google_access_key != nullptr ? opt_google_access_key : "";
    std::string secret_key =
        opt_google_secret_key != nullptr ? opt_google_secret_key : "";
    std::string session_token =
        opt_google_session_token != nullptr ? opt_google_session_token : "";
    std::string storage_class =
        opt_google_storage_class != nullptr ? opt_google_storage_class : "";

    object_store = std::unique_ptr<Object_store>(new S3_object_store(
        &http_client, region, access_key, secret_key, session_token,
        storage_class, opt_max_retries, opt_max_backoff,
        opt_google_endpoint != nullptr ? opt_google_endpoint
                                       : "https://storage.googleapis.com/",
        LOOKUP_DNS, S3_V4));

    if (opt_google_bucket == nullptr) {
      msg_ts("%s: Google bucket is not specified.\n", my_progname);
      return EXIT_FAILURE;
    }

    reinterpret_cast<S3_object_store *>(object_store.get())
        ->set_extra_http_headers(extra_http_headers);

    container_name = opt_google_bucket;

    if (!reinterpret_cast<S3_object_store *>(object_store.get())
             ->probe_api_version_and_lookup(container_name)) {
      return EXIT_FAILURE;
    }
  } else if (opt_storage == AZURE) {
    std::string storage_account =
        opt_azure_account != nullptr ? opt_azure_account : "";
    if (storage_account.empty() && opt_azure_development_storage) {
      storage_account.assign(azure_development_storage_account);
    }
    if (storage_account.empty()) {
      msg_ts("%s: Azure storage account is not specified.\n", my_progname);
      return EXIT_FAILURE;
    }

    std::string access_key =
        opt_azure_access_key != nullptr ? opt_azure_access_key : "";
    if (access_key.empty() && opt_azure_development_storage) {
      access_key.assign(azure_development_access_key);
    }
    if (access_key.empty()) {
      msg_ts("%s: Azure access key is not specified.\n", my_progname);
      return EXIT_FAILURE;
    }

    container_name = opt_azure_container != nullptr ? opt_azure_container : "";
    if (container_name.empty() && opt_azure_development_storage) {
      container_name.assign(azure_development_container);
    }
    if (container_name.empty()) {
      msg_ts("%s: Azure container is not specified.\n", my_progname);
      return EXIT_FAILURE;
    }

    std::string storage_class =
        opt_azure_storage_class != nullptr ? opt_azure_storage_class : "";

    std::string azure_endpoint =
        opt_azure_endpoint != nullptr ? opt_azure_endpoint : "";

    object_store = std::unique_ptr<Object_store>(new Azure_object_store(
        &http_client, storage_account, access_key,
        opt_azure_development_storage, storage_class, opt_max_retries,
        opt_max_backoff, azure_endpoint));

    reinterpret_cast<Azure_object_store *>(object_store.get())
        ->set_extra_http_headers(extra_http_headers);
  }
  /* validation */
  if (opt_threads > 1 && opt_fifo_dir == nullptr && opt_mode != MODE_DELETE) {
    msg_ts(
        "--fifo-streams parameter set to multi-thread using FIFO files. "
        "It requires --fifo-dir to be set.\n");
    return EXIT_FAILURE;
  }
  int rc = EXIT_SUCCESS;
  if (opt_mode == MODE_PROBE) {
    /* By the time we reach this dispatch, probe_api_version_and_lookup
       (S3) / equivalent backend setup has already run as part of the
       object_store construction above. If we got here with non-null
       object_store, the probe succeeded. */
    if (object_store == nullptr) {
      msg_ts("%s: probe: object store setup failed\n", my_progname);
      rc = EXIT_FAILURE;
    } else {
      msg_ts("%s: probe: OK\n", my_progname);
    }
    http_timing::dump_summary();
    return rc;
  }
  if (opt_mode == MODE_PUT) {
    /* PXB-3671 prototype: --multipart-from-file overrides the xbstream pipe
       reading loop with a direct local-file -> multipart upload. */
    if (opt_multipart_from_file != nullptr) {
      if (!xbcloud_put_from_file(object_store.get(), container_name,
                                 backup_name, opt_multipart_from_file)) {
        rc = EXIT_FAILURE;
      }
    } else if (!xbcloud_put(object_store.get(), container_name, backup_name)) {
      rc = EXIT_FAILURE;
    }
  } else if (opt_mode == MODE_GET) {
    if (!xbcloud_download(object_store.get(), container_name, backup_name)) {
      rc = EXIT_FAILURE;
    }
  } else if (opt_mode == MODE_DELETE) {
    if (!xbcloud_delete(object_store.get(), container_name, backup_name)) {
      rc = EXIT_FAILURE;
    }
  } else {
    msg_ts("Unknown command supplied.\n");
    rc = EXIT_FAILURE;
  }

  /* Always dump the HTTP timing summary if instrumentation was on, so
     successful and failed runs both yield diagnostic numbers. */
  http_timing::dump_summary();
  return rc;
}
