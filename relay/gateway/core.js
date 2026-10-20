/*
 * The Kelp gateway's core — design §5.2, §5.3, C4b-plan.md.
 *
 * Everything here is a plain function over bytes: no DOM, no fetch, no state.
 * Both the bootstrap page and the service worker import it (`importScripts` in
 * the worker, a script tag in the page), which is why it keeps the crypto, the
 * CBOR reader and the tree walk in one place and leaves the two edges thin.
 *
 * The rules it implements are libkelp's, re-stated in the browser's language:
 *
 *   content_key = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/content")
 *   relay_token = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/relay")
 *   object      = 0x01 ‖ nonce[12] ‖ AES-256-GCM(plaintext, aad="kelp/v1/object")
 *   object_id   = SHA-256(object bytes)
 *   manifest    = the canonical CBOR of kelp-swarm-wire-spec.md §3
 *
 * An object is verified against its address *before* it is decrypted, so a
 * relay that substitutes one is caught, not trusted.
 */
(function (root) {
    'use strict';

    var AAD = new TextEncoder().encode('kelp/v1/object');
    var ZERO_SALT = new Uint8Array(32);
    var OBJECT_VERSION = 1;
    var TAG_BYTES = 16;
    var NONCE_BYTES = 12;
    // The same ceiling libkelp enforces: an object is at most 1 MiB.
    var MAX_OBJECT_BYTES = 1048576;
    // How many file manifests are read at once while the tree is walked.
    var MANIFEST_CONCURRENCY = 8;
    // A page's HTML file counts as the entry document when the sender said so
    // by MIME type or by name — both are the sender's own words (§5.3).
    var HTML_NAME = /\.html?$/i;
    var INDEX_NAME = /(^|\/)index\.html$/i;

    function fail(message) {
        var error = new Error(message);
        error.kelp = true;
        return error;
    }

    // --- base32 (lowercase, no padding) — bundle ids and object ids ---------

    var B32 = 'abcdefghijklmnopqrstuvwxyz234567';

    function base32Encode(bytes) {
        var out = '';
        var bits = 0;
        var buffer = 0;
        for (var i = 0; i < bytes.length; ++i) {
            buffer = (buffer << 8) | bytes[i];
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                out += B32[(buffer >> bits) & 31];
            }
        }
        if (bits > 0)
            out += B32[(buffer << (5 - bits)) & 31];
        return out;
    }

    function base32Decode(text) {
        var clean = text.toLowerCase();
        var out = new Uint8Array(Math.floor(clean.length * 5 / 8));
        var bits = 0;
        var buffer = 0;
        var used = 0;
        for (var i = 0; i < clean.length; ++i) {
            var value = B32.indexOf(clean[i]);
            if (value < 0)
                throw fail('that is not a Kelp content address');
            buffer = (buffer << 5) | value;
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                out[used++] = (buffer >> bits) & 0xff;
            }
        }
        // A trailing partial group must be zero padding, never a real value.
        if (bits >= 5 || (bits > 0 && (buffer & ((1 << bits) - 1)) !== 0))
            throw fail('that is not a Kelp content address');
        return out.slice(0, used);
    }

    // --- base64url (the fragment, and the token in the relay path) ----------

    function base64UrlDecode(text) {
        var clean = text.replace(/-/g, '+').replace(/_/g, '/');
        while (clean.length % 4 !== 0)
            clean += '=';
        var binary;
        try {
            binary = atob(clean);
        } catch (e) {
            throw fail('that is not a Kelp link fragment');
        }
        var out = new Uint8Array(binary.length);
        for (var i = 0; i < binary.length; ++i)
            out[i] = binary.charCodeAt(i);
        return out;
    }

    function base64UrlEncode(bytes) {
        var binary = '';
        for (var i = 0; i < bytes.length; ++i)
            binary += String.fromCharCode(bytes[i]);
        return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    }

    function equalBytes(a, b) {
        if (a.length !== b.length)
            return false;
        var difference = 0;
        for (var i = 0; i < a.length; ++i)
            difference |= a[i] ^ b[i];
        return difference === 0;
    }

    // --- CRC-32, for the archive ---------------------------------------------
    //
    // The polynomial every ZIP uses (0xEDB88320, reflected). A browser has no
    // standard CRC, so this is it: the archive is only worth anything if its
    // checksums are exactly right.

    var CRC_TABLE = null;

    function crcTable() {
        if (CRC_TABLE)
            return CRC_TABLE;
        var table = new Uint32Array(256);
        for (var n = 0; n < 256; ++n) {
            var c = n;
            for (var bit = 0; bit < 8; ++bit)
                c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
            table[n] = c >>> 0;
        }
        CRC_TABLE = table;
        return table;
    }

    // The CRC of the concatenation of `parts`, as a ZIP stores it.
    function crc32Parts(parts) {
        var table = crcTable();
        var crc = 0xFFFFFFFF;
        for (var i = 0; i < parts.length; ++i) {
            var part = parts[i];
            for (var j = 0; j < part.length; ++j)
                crc = (table[(crc ^ part[j]) & 0xFF] ^ (crc >>> 8)) >>> 0;
        }
        return (crc ^ 0xFFFFFFFF) >>> 0;
    }

    function crc32(bytes) { return crc32Parts([bytes]); }

    // --- the two derivations and the object envelope ------------------------

    function hkdf(secret, info) {
        return crypto.subtle.importKey('raw', secret, 'HKDF', false, ['deriveBits'])
            .then(function (key) {
                return crypto.subtle.deriveBits({
                    name: 'HKDF',
                    hash: 'SHA-256',
                    salt: ZERO_SALT,
                    info: new TextEncoder().encode(info)
                }, key, 256);
            })
            .then(function (bits) { return new Uint8Array(bits); });
    }

    function contentKey(secret) { return hkdf(secret, 'kelp/v1/content'); }
    function relayToken(secret) { return hkdf(secret, 'kelp/v1/relay'); }

    function sha256(bytes) {
        return crypto.subtle.digest('SHA-256', bytes).then(function (digest) {
            return new Uint8Array(digest);
        });
    }

    function openObject(key, objectBytes) {
        if (objectBytes.length < 1 + NONCE_BYTES + TAG_BYTES)
            throw fail('the relay sent something too short to be an object');
        if (objectBytes.length > MAX_OBJECT_BYTES)
            throw fail('the relay sent something larger than an object');
        if (objectBytes[0] !== OBJECT_VERSION)
            throw fail('the relay sent a version of the object format this loader does not know');
        var nonce = objectBytes.subarray(1, 1 + NONCE_BYTES);
        var body = objectBytes.subarray(1 + NONCE_BYTES);
        return crypto.subtle.importKey('raw', key, 'AES-GCM', false, ['decrypt'])
            .then(function (aes) {
                return crypto.subtle.decrypt({
                    name: 'AES-GCM',
                    iv: nonce,
                    additionalData: AAD,
                    tagLength: 128
                }, aes, body);
            })
            .then(function (plain) { return new Uint8Array(plain); })
            .catch(function () {
                // A wrong key and a tampered object land here together, which is
                // the point of a tag.
                throw fail('an object did not open with this link\'s key');
            });
    }

    // --- the link's address block ------------------------------------------
    //
    // The link carries one opaque `q=` block: canonical CBOR `{l, n, p}` —
    // address, label, is-a-page — sealed under a key derived from the *secret*
    // (kelp/v1/link) with the 32-byte bundle id as associated data. It is not
    // sealed to the relay: a reader who holds the fragment can open it, and so
    // can this page. Only the label is needed here (the archive is named after
    // the folder), but the block is opened whole, to libkelp's rules.

    function linkKey(secret) { return hkdf(secret, 'kelp/v1/link'); }

    function openLocations(secret, bundleId, blob) {
        var raw;
        try {
            raw = base64UrlDecode(blob);
        } catch (e) {
            return Promise.reject(fail('the link\'s address block is not base64url'));
        }
        if (raw.length < NONCE_BYTES + TAG_BYTES)
            return Promise.reject(fail('the link\'s address block is too short'));
        return linkKey(secret).then(function (key) {
            return crypto.subtle.importKey('raw', key, 'AES-GCM', false, ['decrypt']);
        }).then(function (aes) {
            return crypto.subtle.decrypt({
                name: 'AES-GCM',
                iv: raw.subarray(0, NONCE_BYTES),
                additionalData: bundleId,
                tagLength: 128
            }, aes, raw.subarray(NONCE_BYTES));
        }).then(function (plain) {
            var map = decodeCbor(new Uint8Array(plain));
            if (!(map instanceof Map))
                throw fail('the link\'s address block is not a map');
            var name = map.get('n');
            var lan = map.get('l');
            var page = map.get('p');
            return {
                name: typeof name === 'string' ? name : '',
                lan: typeof lan === 'string' ? lan : '',
                page: page === true
            };
        }).catch(function (error) {
            throw error && error.message && /address block/.test(error.message)
                ? error
                : fail('the link\'s address block did not open');
        });
    }

    // --- the CBOR the manifests are written in ------------------------------

    function Reader(bytes) {
        this.bytes = bytes;
        this.pos = 0;
    }

    Reader.prototype.need = function (count) {
        if (this.pos + count > this.bytes.length)
            throw fail('a manifest is truncated');
    };

    Reader.prototype.argument = function (additional) {
        if (additional < 24)
            return additional;
        var width = { 24: 1, 25: 2, 26: 4, 27: 8 }[additional];
        if (!width)
            throw fail('a manifest uses a form this loader does not read');
        this.need(width);
        var value = 0;
        for (var i = 0; i < width; ++i)
            value = value * 256 + this.bytes[this.pos++];
        return value;
    };

    Reader.prototype.take = function (count) {
        this.need(count);
        var out = this.bytes.subarray(this.pos, this.pos + count);
        this.pos += count;
        return out;
    };

    Reader.prototype.value = function () {
        this.need(1);
        var first = this.bytes[this.pos++];
        var major = first >> 5;
        var additional = first & 31;
        var i;
        switch (major) {
        case 0:
            return this.argument(additional);
        case 1:
            return -1 - this.argument(additional);
        case 2:
            return this.take(this.argument(additional));
        case 3:
            return new TextDecoder().decode(this.take(this.argument(additional)));
        case 4: {
            var count = this.argument(additional);
            var array = [];
            for (i = 0; i < count; ++i)
                array.push(this.value());
            return array;
        }
        case 5: {
            var pairs = this.argument(additional);
            var map = new Map();
            for (i = 0; i < pairs; ++i) {
                var key = this.value();
                map.set(key, this.value());
            }
            return map;
        }
        case 7:
            if (additional === 20) return false;
            if (additional === 21) return true;
            if (additional === 22) return null;
            throw fail('a manifest has a value this loader does not read');
        default:
            throw fail('a manifest has a form this loader does not read');
        }
    };

    function decodeCbor(bytes) {
        var reader = new Reader(bytes);
        var value = reader.value();
        if (reader.pos !== bytes.length)
            throw fail('a manifest has bytes after its value');
        return value;
    }

    // { "c": [hash…], "mime": text?, "s": uint, "t": "f", "v": 1 }
    function fileManifest(bytes) {
        var map = decodeCbor(bytes);
        if (!(map instanceof Map) || map.get('t') !== 'f' || map.get('v') !== 1)
            throw fail('that is not a file manifest');
        var chunks = map.get('c');
        if (!Array.isArray(chunks))
            throw fail('a file manifest has no chunks');
        return {
            chunks: chunks,
            mime: typeof map.get('mime') === 'string' ? map.get('mime') : '',
            size: Number(map.get('s'))
        };
    }

    function entriesOf(map) {
        var entries = map.get('e');
        if (!Array.isArray(entries))
            throw fail('a directory manifest has no entries');
        return entries.map(function (raw) {
            if (!(raw instanceof Map))
                throw fail('a directory entry is not a map');
            return {
                name: raw.get('n'),
                isDir: raw.get('t') === 'd',
                hash: raw.get('h'),
                size: Number(raw.get('s'))
            };
        });
    }

    // --- the tree -----------------------------------------------------------

    // The directory manifests form a tree whose leaves are file manifests. An
    // index-form directory ("c") lists child manifests in order; a leaf-form one
    // ("e") lists entries directly. Both are walked here, and each file's own
    // manifest is read once for its name, size and MIME.
    //
    // `fetcher(hash)` returns the *plaintext* of the object with that id, having
    // already verified and decrypted it.
    function walk(fetcher, rootHash, onProgress) {
        var files = [];
        var directories = 0;
        var dirSet = {};
        var seen = {};

        function visit(hash, path) {
            var key = base32Encode(hash);
            if (seen[key])
                return Promise.resolve();
            seen[key] = true;
            return fetcher(hash).then(function (plain) {
                var map = decodeCbor(plain);
                if (!(map instanceof Map) || map.get('v') !== 1 || map.get('t') !== 'd')
                    throw fail('a directory manifest is not a directory');
                ++directories;
                // A directory's own path, for the archive's folder entries. The
                // root is the archive itself; an index-form directory ("c") is
                // visited once per chunk at the same path, so this is a set.
                if (path)
                    dirSet[path.replace(/\/$/, '')] = true;
                var children = [];
                if (map.has('e')) {
                    entriesOf(map).forEach(function (entry) {
                        var child = path + entry.name;
                        if (entry.isDir)
                            children.push(visit(entry.hash, child + '/'));
                        else
                            files.push({ path: child, hash: entry.hash, sizeMillis: entry.size });
                    });
                    return Promise.all(children);
                }
                var listed = map.get('c');
                if (!Array.isArray(listed))
                    throw fail('a directory manifest is neither entries nor children');
                listed.forEach(function (child) {
                    children.push(visit(child, path));
                });
                return Promise.all(children);
            });
        }

        return visit(rootHash, '').then(function () {
            // Every file's manifest, read with a small working set, so a deep
            // tree does not mean a thousand sockets.
            var index = 0;
            var done = 0;

            function worker() {
                if (index >= files.length)
                    return Promise.resolve();
                var file = files[index++];
                return fetcher(file.hash).then(function (plain) {
                    var manifest = fileManifest(plain);
                    file.mime = manifest.mime;
                    file.size = manifest.size;
                    file.chunks = manifest.chunks;
                    delete file.sizeMillis;
                }).then(function () {
                    ++done;
                    if (onProgress)
                        onProgress(done, files.length);
                    return worker();
                });
            }

            var running = [];
            for (var i = 0; i < MANIFEST_CONCURRENCY; ++i)
                running.push(worker());
            return Promise.all(running);
        }).then(function () {
            files.sort(function (a, b) { return a.path < b.path ? -1 : (a.path > b.path ? 1 : 0); });
            return { files: files, directories: directories, dirs: Object.keys(dirSet).sort() };
        });
    }

    // --- which file is the page (§5.3) --------------------------------------
    //
    // The one copy of this rule in C++ is libkelp/kelp_web.cpp (isHtmlName,
    // chooseEntry, mimeType); what is here is its mirror, so that a page a
    // recipient's app previews and the same page a browser sees through the
    // gateway cannot be two different files.

    function isHtml(file) {
        if ((file.mime || '').toLowerCase().indexOf('text/html') === 0)
            return true;
        return HTML_NAME.test(file.path);
    }

    function depth(path) {
        var count = 0;
        for (var i = 0; i < path.length; ++i) {
            if (path[i] === '/')
                ++count;
        }
        return count;
    }

    function fileName(path) {
        var cut = path.lastIndexOf('/');
        return cut < 0 ? path : path.slice(cut + 1);
    }

    // "index.html" at the bundle root, else any other index.html (the shallowest),
    // then the only HTML file there is. Anything else is a choice the recipient
    // makes, so it is left to them: the caller shows the file list.
    function chooseEntry(files) {
        var html = files.filter(isHtml).slice().sort(function (a, b) {
            return a.path < b.path ? -1 : (a.path > b.path ? 1 : 0);
        });
        if (html.length === 0)
            return null;

        var i;
        for (i = 0; i < html.length; ++i) {
            // The root test is case-insensitive but the entry is the path that
            // actually matched, not the literal "index.html".
            if (html[i].path.toLowerCase() === 'index.html')
                return html[i];
        }

        var indexes = html.filter(function (file) {
            return fileName(file.path).toLowerCase() === 'index.html';
        });
        if (indexes.length > 0) {
            var best = indexes[0];
            for (i = 1; i < indexes.length; ++i) {
                if (depth(indexes[i].path) < depth(best.path))
                    best = indexes[i];
            }
            return best;
        }
        return html.length === 1 ? html[0] : null;
    }

    // The chosen file's directory. libkelp's Web::root carries no trailing slash;
    // this one does, because it is then a prefix to slice with.
    function webRootOf(entry) {
        var cut = entry.path.lastIndexOf('/');
        return cut < 0 ? '' : entry.path.slice(0, cut + 1);
    }

    // The bundle's paths relative to its web root, which is the URL space the
    // service worker answers in: "index.html" is "/", "sub/x.png" is "/sub/x.png".
    function urlSpace(files, webRoot) {
        var space = {};
        files.forEach(function (file) {
            if (file.path.indexOf(webRoot) !== 0)
                return;
            var url = file.path.slice(webRoot.length);
            if (url.length > 0)
                space[url] = file;
        });
        return space;
    }

    // A path in the bundle's URL space, resolved the way a web root resolves one:
    // nothing (or "/") is the entry document, a path ending in a slash is that
    // directory's index.html, a name that is not there may still be a directory
    // whose index.html is. `space` is urlSpace()'s answer, so the keys are the
    // URLs; null means the bundle has nothing there — a 404, never the relay.
    function resolvePath(space, entry, relative) {
        if (relative === '' || relative === '/')
            return entry;
        if (relative.charAt(relative.length - 1) === '/') {
            var index = relative + 'index.html';
            return space[index] ? index : null;
        }
        if (space[relative])
            return relative;
        var directoryIndex = relative + '/index.html';
        return space[directoryIndex] ? directoryIndex : null;
    }

    // A MIME type for the browser when the sender recorded none — which is every
    // bundle the app seals today (nothing sets `mime`), so this table is the
    // common path, not a fallback. It is the same table as libkelp/kelp_web.cpp's
    // mimeType(). A wrong guess is a broken page, never a security hole.
    var BY_EXTENSION = {
        html: 'text/html; charset=utf-8', htm: 'text/html; charset=utf-8',
        css: 'text/css; charset=utf-8', js: 'text/javascript; charset=utf-8',
        mjs: 'text/javascript; charset=utf-8', json: 'application/json; charset=utf-8',
        map: 'application/json; charset=utf-8',
        webmanifest: 'application/manifest+json; charset=utf-8',
        txt: 'text/plain; charset=utf-8', md: 'text/plain; charset=utf-8',
        csv: 'text/csv; charset=utf-8', xml: 'text/xml; charset=utf-8',
        svg: 'image/svg+xml', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg',
        gif: 'image/gif', webp: 'image/webp', ico: 'image/x-icon', bmp: 'image/bmp',
        pdf: 'application/pdf', mp3: 'audio/mpeg', wav: 'audio/wav',
        ogg: 'audio/ogg', oga: 'audio/ogg', mp4: 'video/mp4', m4v: 'video/mp4',
        webm: 'video/webm', ogv: 'video/ogg', woff: 'font/woff', woff2: 'font/woff2',
        ttf: 'font/ttf', otf: 'font/otf', wasm: 'application/wasm',
        zip: 'application/zip'
    };

    function mimeFor(file) {
        if (file.mime)
            return file.mime;
        var name = file.path.toLowerCase();
        var dot = name.lastIndexOf('.');
        var extension = dot < 0 ? '' : name.slice(dot + 1);
        return BY_EXTENSION[extension] || 'application/octet-stream';
    }

    // --- the relay's own paths ----------------------------------------------

    function apiBase(bundleId, token) {
        return '/v1/relay/' + base64UrlEncode(token) + '/bundle/' + base32Encode(bundleId);
    }

    function objectUrl(bundleId, token, hash) {
        return apiBase(bundleId, token) + '/object/' + base32Encode(hash);
    }

    // --- the bootstrap's handover to the worker ------------------------------
    //
    // Both keys travel as the bytes they are. apiBase() encodes the relay token
    // itself, so handing its base64url text across would encode it a second time
    // and the relay would answer 401 — which is exactly what happened the first
    // time this ran in a real browser, and the reason this is a function with a
    // test rather than two literals at either end that have to agree.

    function sessionMessage(bundleIdText, keys, entryUrl, files) {
        return {
            type: 'kelp-session',
            bundleId: bundleIdText,
            token: keys.relayToken,
            key: keys.contentKey,
            entry: entryUrl,
            files: files
        };
    }

    // The worker's side of it: one fetcher for the whole bundle, built from the
    // session the bootstrap posted.
    function sessionFetcher(session, doFetch) {
        return makeFetcher(base32Decode(session.bundleId), session.token, session.key,
                           doFetch);
    }

    // The files a worker serves: every URL in the bundle's space, each named by
    // the object ids of its *chunks*, in order. A file's own address is its
    // manifest's address, and a manifest is not the file — handing that to the
    // worker made every page serve its own manifest as HTML, which is the second
    // bug the emulator found. Chunks in order also mean a file longer than one
    // chunk arrives whole.
    function pageFiles(files, webRoot) {
        var space = urlSpace(files, webRoot);
        var out = {};
        Object.keys(space).forEach(function (url) {
            var file = space[url];
            out[url] = {
                chunks: file.chunks.map(base32Encode),
                mime: mimeFor(file),
                size: file.size
            };
        });
        return out;
    }

    function pageSession(bundleIdText, keys, tree, entry) {
        var webRoot = webRootOf(entry);
        return sessionMessage(bundleIdText, keys, entry.path.slice(webRoot.length),
                              pageFiles(tree.files, webRoot));
    }

    // `doFetch` is passed in rather than reached for, so the Node tests can drive
    // the walk from a fixture with no network, and the browser code is honest
    // about its one input.
    function makeFetcher(bundleId, token, key, doFetch) {
        return function (hash) {
            return fetchObject(objectUrl(bundleId, token, hash), doFetch).then(function (response) {
                if (!response.ok)
                    throw fail('the relay answered ' + response.status + ' for an object');
                return response.arrayBuffer();
            }).then(function (buffer) {
                var bytes = new Uint8Array(buffer);
                return sha256(bytes).then(function (digest) {
                    // Verified before it is decrypted: a relay that substitutes an
                    // object is caught here, not trusted (wire spec §12.1).
                    if (!equalBytes(digest, hash))
                        throw fail('the relay sent an object that does not match its address');
                    return openObject(key, bytes);
                });
            });
        };
    }

    // The relay serves one object at a time and answers 503 when the same object
    // is already in flight for someone else ("that object is already in flight").
    // A second reader is not an error: wait a moment and ask again, once.
    function fetchObject(url, doFetch) {
        return doFetch(url).then(function (response) {
            if (response.status !== 503)
                return response;
            return new Promise(function (resolve, reject) {
                setTimeout(function () {
                    doFetch(url).then(resolve, reject);
                }, 400);
            });
        });
    }

    function fullTree(bundleId, token, key, doFetch, onProgress) {
        return walk(makeFetcher(bundleId, token, key, doFetch), bundleId, onProgress);
    }

    // Both derivations the link's secret yields, once, before anything is asked
    // of the relay: the content key never leaves this side, the relay token is
    // the only thing that does.
    function prepare(secret) {
        return Promise.all([contentKey(secret), relayToken(secret)]).then(function (both) {
            return { contentKey: both[0], relayToken: both[1] };
        });
    }

    // --- the archive ---------------------------------------------------------
    //
    // A bundle with no page is a folder; "Download folder" hands it over as one
    // stored (uncompressed) ZIP. Built here, in the page: the relay holds
    // ciphertext only and never sees a key (design §5.2), so a server-side
    // archive is the one thing that must not exist.

    // A classic ZIP's ceilings, which keep every archive clear of ZIP64: a
    // download that needed ZIP64 needs a different design (streaming), and
    // saying so is better than running out of memory halfway.
    var ZIP_MAX_BYTES = 128 * 1024 * 1024;
    var ZIP_MAX_ENTRIES = 65535;

    var DOS_DATE = 0x0021;     // 1980-01-01, the DOS epoch: no clock in the bytes
    var DOS_TIME = 0;

    function utf8(text) { return new TextEncoder().encode(text); }

    // One path component a filesystem will take: no control characters, no
    // backslash (Windows reads it as a separator), no "."/"..", no trailing dot
    // or space (Windows drops them, which can merge two names), no drive prefix,
    // at most 255 bytes of UTF-8.
    function safeComponent(part) {
        var text = String(part);
        if (text.normalize)
            text = text.normalize('NFC');
        text = text.replace(/[\u0000-\u001f\u007f\\]/g, '_');
        if (text === '.' || text === '..')
            text = '_';
        text = text.replace(/[. ]+$/, '');
        if (/^[A-Za-z]:/.test(text))
            text = '_' + text;
        if (!text)
            text = '_';
        if (utf8(text).length > 255) {
            var kept = '';
            var used = 0;
            for (var i = 0; i < text.length; ++i) {
                var width = 1;
                var code = text.charCodeAt(i);
                if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length)
                    width = 2;
                var piece = text.substr(i, width);
                var size = utf8(piece).length;
                if (used + size > 255)
                    break;
                kept += piece;
                used += size;
                i += width - 1;
            }
            text = kept || '_';
        }
        return text;
    }

    // A whole relative path, every component checked. Empty components (a
    // leading slash, "a//b") are dropped rather than turned into "_": they name
    // nothing.
    function zipSafeName(path) {
        var pieces = String(path).split('/');
        var out = [];
        for (var i = 0; i < pieces.length; ++i) {
            if (pieces[i])
                out.push(safeComponent(pieces[i]));
        }
        return out.join('/');
    }

    function numbered(base, n, isDir) {
        if (isDir)
            return base + ' (' + n + ')';
        var dot = base.lastIndexOf('.');
        return dot > 0 ? base.slice(0, dot) + ' (' + n + ')' + base.slice(dot)
                       : base + ' (' + n + ')';
    }

    // Entry names for an archive: sanitised, then made unique in one namespace
    // (a folder and a file may not share a name) case-insensitively, because
    // Windows and macOS extract case-insensitively.
    function zipNames(files, dirs) {
        var used = Object.create(null);
        var names = { files: [], dirs: [] };
        function take(base, isDir) {
            var candidate = base || '_';
            var n = 2;
            while (used[candidate.toLowerCase()])
                candidate = numbered(base || '_', n++, isDir);
            used[candidate.toLowerCase()] = true;
            return candidate;
        }
        (dirs || []).forEach(function (dir) {
            names.dirs.push(take(zipSafeName(dir), true));
        });
        files.forEach(function (file) {
            names.files.push(take(zipSafeName(file.path), false));
        });
        return names;
    }

    function le16(value) {
        return [value & 0xFF, (value >>> 8) & 0xFF];
    }

    function le32(value) {
        return [value & 0xFF, (value >>> 8) & 0xFF,
                (value >>> 16) & 0xFF, (value >>> 24) & 0xFF];
    }

    function concat(arrays) {
        var total = 0;
        var i;
        for (i = 0; i < arrays.length; ++i)
            total += arrays[i].length;
        var whole = new Uint8Array(total);
        var at = 0;
        for (i = 0; i < arrays.length; ++i) {
            whole.set(arrays[i], at);
            at += arrays[i].length;
        }
        return whole;
    }

    // A stored-ZIP writer over byte arrays. `add` computes the CRC and the size,
    // writes the local header and keeps references to the data (no copy); the
    // caller turns `finish()`'s parts into one Blob.
    function zipWriter() {
        var parts = [];
        var central = [];
        var offset = 0;
        var entries = 0;
        var fileBytes = 0;

        function entry(name, crc, size, isDir) {
            var nameBytes = utf8(isDir ? name + '/' : name);
            if (nameBytes.length > 0xFFFF)
                throw fail('a name in this folder is too long for one archive');
            var header = new Uint8Array(concat([
                le32(0x04034B50), le16(20), le16(0x0800), le16(0),
                le16(DOS_TIME), le16(DOS_DATE),
                le32(crc), le32(size), le32(size),
                le16(nameBytes.length), le16(0),
                nameBytes
            ]));
            if (offset + header.length + size > 0xFFFFFFFF)
                throw fail('the archive would need ZIP64');
            parts.push(header);
            central.push({ name: nameBytes, crc: crc, size: size, offset: offset });
            offset += header.length + size;
            ++entries;
        }

        return {
            add: function (name, chunks) {
                var size = 0;
                for (var i = 0; i < chunks.length; ++i)
                    size += chunks[i].length;
                if (size > 0xFFFFFFFF)
                    throw fail('a file in this folder is too large for one archive');
                entry(name, crc32Parts(chunks), size, false);
                for (var j = 0; j < chunks.length; ++j)
                    parts.push(chunks[j]);
                fileBytes += size;
            },
            addDirectory: function (name) {
                entry(name, 0, 0, true);
            },
            finish: function () {
                if (entries > ZIP_MAX_ENTRIES || central.length > 0xFFFF)
                    throw fail('too many entries for one archive');
                var start = offset;
                for (var i = 0; i < central.length; ++i) {
                    var c = central[i];
                    var header = new Uint8Array(concat([
                        le32(0x02014B50), le16(20), le16(20), le16(0x0800), le16(0),
                        le16(DOS_TIME), le16(DOS_DATE),
                        le32(c.crc), le32(c.size), le32(c.size),
                        le16(c.name.length), le16(0), le16(0),
                        le16(0), le16(0), le32(0),
                        le32(c.offset), c.name
                    ]));
                    parts.push(header);
                    offset += header.length;
                }
                var centralSize = offset - start;
                if (offset > 0xFFFFFFFF || centralSize > 0xFFFFFFFF)
                    throw fail('the archive would need ZIP64');
                parts.push(new Uint8Array(concat([
                    le32(0x06054B50), le16(0), le16(0),
                    le16(central.length), le16(central.length),
                    le32(centralSize), le32(start), le16(0)
                ])));
                return { parts: parts, bytes: fileBytes, entries: entries };
            }
        };
    }

    // The whole folder as one archive. Files are fetched one at a time (chunks in
    // parallel inside a file) so the working set is one file, not the bundle;
    // `cancelled()` is asked between files, and a cancellation, a capped bundle
    // or a bad byte count rejects — a partial archive is never returned.
    function zipBundle(fetcher, files, dirs, options) {
        options = options || {};
        var totalBytes = 0;
        for (var i = 0; i < files.length; ++i)
            totalBytes += files[i].size;
        var entryCount = files.length + (dirs ? dirs.length : 0);
        if (entryCount > ZIP_MAX_ENTRIES)
            return Promise.reject(fail('this folder has too many files for one archive'));
        if (totalBytes > ZIP_MAX_BYTES)
            return Promise.reject(fail('this folder is too large for one archive'));

        var names = zipNames(files, dirs || []);
        var writer = zipWriter();
        (dirs || []).forEach(function (dir, at) { writer.addDirectory(names.dirs[at]); });

        var index = 0;
        var done = 0;
        var bytesDone = 0;

        function step() {
            if (options.cancelled && options.cancelled()) {
                var stopped = fail('cancelled');
                stopped.cancelled = true;
                return Promise.reject(stopped);
            }
            if (index >= files.length)
                return Promise.resolve();
            var at = index++;
            var file = files[at];
            return Promise.all(file.chunks.map(fetcher)).then(function (chunks) {
                var got = 0;
                for (var k = 0; k < chunks.length; ++k)
                    got += chunks[k].length;
                if (got !== file.size)
                    throw fail('the bytes fetched for ' + file.path
                               + ' do not match what the bundle says');
                writer.add(names.files[at], chunks);
                ++done;
                bytesDone += got;
                if (options.onProgress)
                    options.onProgress(done, files.length, bytesDone, totalBytes);
                return step();
            });
        }

        return step().then(function () { return writer.finish(); });
    }

    root.KelpGateway = {
        base32Encode: base32Encode,
        base32Decode: base32Decode,
        base64UrlEncode: base64UrlEncode,
        base64UrlDecode: base64UrlDecode,
        equalBytes: equalBytes,
        sha256: sha256,
        contentKey: contentKey,
        relayToken: relayToken,
        openObject: openObject,
        decodeCbor: decodeCbor,
        fileManifest: fileManifest,
        walk: walk,
        chooseEntry: chooseEntry,
        webRootOf: webRootOf,
        urlSpace: urlSpace,
        resolvePath: resolvePath,
        mimeFor: mimeFor,
        isHtml: isHtml,
        apiBase: apiBase,
        objectUrl: objectUrl,
        sessionMessage: sessionMessage,
        sessionFetcher: sessionFetcher,
        pageFiles: pageFiles,
        pageSession: pageSession,
        makeFetcher: makeFetcher,
        prepare: prepare,
        fullTree: fullTree,
        crc32: crc32,
        crc32Parts: crc32Parts,
        linkKey: linkKey,
        openLocations: openLocations,
        zipSafeName: zipSafeName,
        zipNames: zipNames,
        zipWriter: zipWriter,
        zipBundle: zipBundle,
        ZIP_MAX_BYTES: ZIP_MAX_BYTES,
        ZIP_MAX_ENTRIES: ZIP_MAX_ENTRIES,
        MAX_OBJECT_BYTES: MAX_OBJECT_BYTES
    };
}(typeof self !== 'undefined' ? self : this));
