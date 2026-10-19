/*
 * The gateway's service worker — design §5.2, §5.3, C4b-plan.md.
 *
 * A page's subresources, fetches and XHR all come through here, because the
 * page lives in the bundle's own URL space under /b/<bundle-id>/ (§5.3). For
 * every request in that space it
 *
 *   1. resolves the path to an object id (directories become index.html),
 *   2. fetches the ciphertext from the relay, on the link's own relay token,
 *   3. verifies it against its address *before* decrypting,
 *   4. answers with the decrypted bytes and the MIME the sender recorded.
 *
 * The key lives only in this worker's memory: it arrives by postMessage from the
 * bootstrap, which read it from the fragment, and it is gone when the worker is.
 * A worker that has no session (a new tab, a restarted browser) stays out of the
 * way, so the navigation reaches the bootstrap and the fragment is read again.
 */
importScripts('./core.js');

var G = self.KelpGateway;
var session = null;

// The URL space this worker answers in: /b/<bundle-id>/ — its own scope, which
// the relay asserts with Service-Worker-Allowed.
var prefix = new URL(self.registration.scope).pathname;

self.addEventListener('install', function () {
    self.skipWaiting();
});

self.addEventListener('activate', function (event) {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('message', function (event) {
    var data = event.data;
    if (data && data.type === 'kelp-session') {
        session = data;
    } else if (data && data.type === 'kelp-forget') {
        session = null;
    }
});

function respondWith(status, reason, contentType, body) {
    return new Response(body, {
        status: status,
        statusText: reason,
        headers: { 'Content-Type': contentType, 'Cache-Control': 'no-store' }
    });
}

// A path in the bundle's space, or null when the bundle has nothing there.
function resolvePath(relative) {
    return G.resolvePath(session.files, session.entry, relative);
}

function serve(file) {
    // The core's fetcher does the whole job: ask the relay on the link's own
    // relay token, verify the bytes against the object id *before* decrypting,
    // and hand back the plaintext (it retries once when the relay says the
    // object is already in flight for another reader).
    var fetcher = G.makeFetcher(G.base32Decode(session.bundleId), session.token,
                               session.key, fetch);
    return fetcher(G.base32Decode(file.id)).then(function (plain) {
        return new Response(plain, {
            status: 200,
            headers: {
                'Content-Type': file.mime || 'application/octet-stream',
                'Cache-Control': 'no-store'
            }
        });
    }).catch(function (error) {
        return respondWith(502, 'Bad Gateway', 'text/plain; charset=utf-8',
                           'This file could not be fetched: '
                           + (error && error.message ? error.message : error) + '\n');
    });
}

self.addEventListener('fetch', function (event) {
    var url = new URL(event.request.url);
    if (url.origin !== self.location.origin)
        return;
    if (url.pathname.indexOf(prefix) !== 0)
        return;
    if (event.request.method !== 'GET' && event.request.method !== 'HEAD')
        return;

    var relative = url.pathname.slice(prefix.length);

    // The gateway's own files are the relay's, not the bundle's.
    if (relative.indexOf('.kelp/') === 0 || relative === '.kelp')
        return;

    // No key yet: let the navigation reach the bootstrap, which reads the
    // fragment and posts a session back here.
    if (!session)
        return;

    var path = resolvePath(relative);
    if (!path) {
        event.respondWith(respondWith(404, 'Not Found', 'text/plain; charset=utf-8',
                                      'This bundle has nothing at that path.\n'));
        return;
    }
    event.respondWith(serve(session.files[path]));
});
