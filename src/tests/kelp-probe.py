#!/usr/bin/env python3
"""
kelp-probe.py — ask a running Kelp server whether it really serves a link.

Give it the publisher's log on stdin: it picks up the last link the app minted,
derives the bearer token the way libkelp does (HKDF-SHA256, `kelp/v1/swarm`), and
makes the same requests a recipient would —

  * an authenticated `/info`,
  * an authenticated GET of the root object,
  * an `/info` with no token, and one with the wrong token, both of which must be
    refused.

— printing status codes and sizes only. A link's `#k=` fragment *is* the
decryption key, so it is parsed here and never printed: the output is safe to
read, paste, or screenshot. Nothing is decrypted; this checks reachability,
authorisation and that objects leave the device, which is everything except the
recipient's own crypto.

Usage:
    kelp-probe.py --base http://HOST:PORT [--link NUMBER] < kelp.log

    --base         the address the share can be reached on, e.g. through an ssh
                   forward set up by run-emu-share.sh.
    --link NUMBER  which link to use, counting back from the last one in the log
                   (default 1, the newest).

Exit status is 0 only if every check passed, so a caller can branch on it.
"""

import argparse
import base64
import hashlib
import hmac
import re
import sys
import urllib.error
import urllib.request


def base64url_decode(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def base64url_encode(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def hkdf_sha256(ikm, info, length=32):
    """
    HKDF-SHA256 with the 32 zero-byte salt libkelp uses (libkelp/kelp_bundle.cpp):
    extract, then expand one 32-byte block, which is all a 32-byte token needs.
    """
    salt = bytes(32)
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    return hmac.new(prk, info + bytes([1]), hashlib.sha256).digest()[:length]


def get(base, path, token=None, timeout=20):
    request = urllib.request.Request(base + path)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as reply:
            return reply.status, reply.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()
    except (urllib.error.URLError, OSError) as error:
        return 0, str(error).encode()


def describe(payload, limit=60):
    """The short form of a reply body, for a one-line report."""
    text = payload[:limit].decode(errors="replace").strip()
    return text or "(empty)"


def main():
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--base", required=True, help="http://HOST:PORT")
    parser.add_argument("--link", type=int, default=1)
    args = parser.parse_args()

    base = args.base.rstrip("/")
    links = re.findall(r"kelp://[^\"\s]+", sys.stdin.read())
    if len(links) < args.link:
        print("the log holds %d link(s); asked for number %d"
              % (len(links), args.link))
        return 1
    link = links[-args.link]

    # kelp://<bundle-id>?<sealed address and name>#k=<secret>
    secret = base64url_decode(link.split("#k=", 1)[1])
    bundle_id = link.split("#", 1)[0][len("kelp://"):].split("?")[0]
    token = base64url_encode(hkdf_sha256(secret, b"kelp/v1/swarm"))

    print("bundle %s…%s, %d-byte secret, %d-byte token"
          % (bundle_id[:8], bundle_id[-4:], len(secret), len(token)))
    print("address %s" % base)

    ok = True

    status, payload = get(base, "/v1/bundle/%s/info" % bundle_id, token)
    print("  /info           right token -> %d %s" % (status, describe(payload)))
    ok = ok and status == 200 and b'"objects"' in payload

    status, payload = get(base, "/v1/bundle/%s/object/%s" % (bundle_id, bundle_id),
                          token)
    if status == 200:
        print("  /object/<root>  right token -> 200, %d byte(s)" % len(payload))
    else:
        print("  /object/<root>  right token -> %d %s" % (status, describe(payload)))
    ok = ok and status == 200 and len(payload) > 0

    status, _ = get(base, "/v1/bundle/%s/info" % bundle_id)
    print("  /info           no token    -> %d (want 401)" % status)
    ok = ok and status == 401

    status, _ = get(base, "/v1/bundle/%s/info" % bundle_id, base64url_encode(bytes(32)))
    print("  /info           wrong token -> %d (want 401)" % status)
    ok = ok and status == 401

    print("result: %s" % ("serves" if ok else "NOT SERVING"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
