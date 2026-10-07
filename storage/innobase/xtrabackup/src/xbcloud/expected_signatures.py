#!/usr/bin/env python3
"""Check the expected signatures of the xbcloud-t signer tests with the
providers' own signing code.

xbcloud-t, run with XBCLOUD_DUMP_SIGNED=<file>, writes every request a
signer test signed: method, URL, all headers including xbcloud's
Authorization, payload and the test credentials. This script signs each of
those requests again with

  s3v4   botocore SigV4Auth (the signing code of the AWS CLI and boto3;
         Google Cloud Storage uses the same signature with HMAC keys)
  s3v2   botocore HmacV1Auth
  azure  azure-storage-blob SharedKeyCredentialPolicy

and compares the result with xbcloud's Authorization header. A match means
xbcloud signs that request correctly; the value printed is the one to put in
xbcloud-t.cc. It says nothing about whether the provider accepts the headers
xbcloud chose: that needs a run against the real service.

usage: expected_signatures.py <file written by xbcloud-t>
needs: pip install botocore azure-storage-blob   (see UNIT_TESTS.md)
"""

import base64
import json
import sys
from urllib.parse import urlsplit


def s3v4(r, headers, payload):
    from botocore.auth import SigV4Auth
    from botocore.awsrequest import AWSRequest
    from botocore.credentials import Credentials
    c = r['credentials']
    req = AWSRequest(method=r['method'], url=r['url'], data=payload,
                     headers=headers)
    req.context['timestamp'] = headers['X-Amz-Date']
    auth = SigV4Auth(Credentials(c['access_key'], c['secret_key']), 's3',
                     c['region'])
    canonical = auth.canonical_request(req)
    signature = auth.signature(auth.string_to_sign(req, canonical), req)
    signed = auth.signed_headers(auth.headers_to_sign(req))
    return 'AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s' % (
        c['access_key'], auth.scope(req).split('/', 1)[1], signed, signature)


def s3v2(r, headers, payload):
    from botocore.auth import HmacV1Auth
    from botocore.credentials import Credentials
    c = r['credentials']
    path = urlsplit(r['url']).path
    bucket = c.get('bucket', '')
    auth_path = '/' + bucket + path if bucket else path
    class FixedDateHmacV1Auth(HmacV1Auth):
        # botocore stamps the current time; sign with the request's Date
        def _get_date(self):
            return headers['Date']

    auth = FixedDateHmacV1Auth(Credentials(c['access_key'], c['secret_key']))
    signature = auth.get_signature(r['method'], urlsplit(r['url']),
                                   dict(headers), auth_path=auth_path)
    return 'AWS %s:%s' % (c['access_key'], signature)


def azure(r, headers, payload):
    from azure.core.pipeline import PipelineContext, PipelineRequest
    from azure.core.rest import HttpRequest
    from azure.storage.blob._shared.authentication import \
        SharedKeyCredentialPolicy
    c = r['credentials']
    req = HttpRequest(r['method'], r['url'], headers=headers, content=payload)
    SharedKeyCredentialPolicy(c['account'], c['key']).on_request(
        PipelineRequest(req, PipelineContext(None)))
    return req.headers['Authorization']


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sign = {'s3v4': s3v4, 's3v2': s3v2, 'azure': azure}
    mismatches = 0
    for line in open(sys.argv[1]):
        r = json.loads(line)
        headers = dict(r['headers'])
        xbcloud = headers.pop('Authorization')
        payload = base64.b64decode(r['payload_base64'])
        expected = sign[r['scheme']](r, headers, payload)
        ok = expected == xbcloud
        mismatches += not ok
        print('%-6s %s\n  SDK:     %s\n  xbcloud: %s\n' % (
            'OK' if ok else 'DIFFER', r['test'], expected,
            'same' if ok else xbcloud))
    print('%d request(s) checked, %d differ' % (
        sum(1 for _ in open(sys.argv[1])), mismatches))
    return 1 if mismatches else 0


if __name__ == '__main__':
    sys.exit(main())
