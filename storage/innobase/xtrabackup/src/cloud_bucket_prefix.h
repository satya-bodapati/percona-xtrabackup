/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

cloud_bucket_prefix.h: parse a "BUCKET" or "BUCKET/PREFIX" string
into (bucket, prefix), with HNS-safe normalization on the prefix.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#ifndef XB_CLOUD_BUCKET_PREFIX_H
#define XB_CLOUD_BUCKET_PREFIX_H

#include <string>

namespace xtrabackup {

/* Split "BUCKET" or "BUCKET/PREFIX[/MORE]" into (bucket, prefix).

   - Splits on the FIRST '/'.  Anything after that is the prefix; the
     prefix may itself contain '/' characters (sub-prefixes).
   - Strips leading AND trailing '/' from the prefix.  This matters on
     HNS-enabled Azure containers: a blob key ending in '/' is
     interpreted as a directory placeholder rather than a blob, which
     would break our per-file upload model.  Stripping the trailing '/'
     here means we never end up PUTting such a key.
   - No-slash input -> bucket is the whole value, prefix is empty.

   Example mappings:
     "mybucket"                -> ("mybucket", "")
     "mybucket/"               -> ("mybucket", "")
     "mybucket/foo"            -> ("mybucket", "foo")
     "mybucket/foo/"           -> ("mybucket", "foo")
     "mybucket/foo/bar"        -> ("mybucket", "foo/bar")
     "mybucket/foo/bar/"       -> ("mybucket", "foo/bar")
     "mybucket///foo///bar///" -> ("mybucket", "foo///bar")     (only the OUTER
                                                                 leading + trailing
                                                                 strips are HNS-
                                                                 relevant; embedded
                                                                 slashes are
                                                                 user-visible sub-
                                                                 prefixes and we
                                                                 leave them alone)
*/
inline void parse_cloud_bucket_with_prefix(const std::string &value,
                                           std::string &bucket,
                                           std::string &prefix) {
  size_t slash = value.find('/');
  if (slash == std::string::npos) {
    bucket = value;
    prefix.clear();
    return;
  }
  bucket = value.substr(0, slash);
  prefix = value.substr(slash + 1);
  while (!prefix.empty() && prefix.front() == '/') prefix.erase(0, 1);
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
}

}  // namespace xtrabackup

#endif  /* XB_CLOUD_BUCKET_PREFIX_H */
