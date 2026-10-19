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
            return { files: files, directories: directories };
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
        makeFetcher: makeFetcher,
        prepare: prepare,
        fullTree: fullTree,
        MAX_OBJECT_BYTES: MAX_OBJECT_BYTES
    };
}(typeof self !== 'undefined' ? self : this));
