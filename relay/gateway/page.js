/*
 * The gateway's bootstrap — design §5.2, §5.3, C4b-plan.md.
 *
 * It runs at /b/<bundle-id>/ with the key in location.hash. It derives the two
 * keys from that secret, walks the bundle's tree through the relay, and then
 * either
 *
 *   - hands the tree and the key to a service worker and reloads, so the
 *     sender's own entry document is served under this URL space (§5.3), or
 *   - shows a file list, when the bundle has no page to be (the plan's D7).
 *
 * The key is used here and in the worker; it is never fetched with, written to
 * disk, or put in a URL that goes to a server.
 */
(function () {
    'use strict';

    var G = window.KelpGateway;
    var statusEl = document.getElementById('status');
    var filesEl = document.getElementById('files');

    function setStatus(text, kind) {
        statusEl.textContent = text;
        statusEl.className = kind || '';
    }

    function showError(message) {
        setStatus(message, 'error');
    }

    function humanBytes(bytes) {
        if (bytes < 1024)
            return bytes + ' B';
        var units = ['KiB', 'MiB', 'GiB', 'TiB'];
        var value = bytes / 1024;
        var unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            ++unit;
        }
        return (value < 10 ? value.toFixed(1) : Math.round(value)) + ' ' + units[unit];
    }

    // --- the link ----------------------------------------------------------

    var match = /^\/b\/([A-Za-z2-7]+)\/?$/.exec(location.pathname);
    if (!match) {
        showError('This is not a Kelp gateway address.');
        return;
    }
    var bundleIdText = match[1].toLowerCase();
    var prefix = '/b/' + bundleIdText + '/';

    var bundleId;
    try {
        bundleId = G.base32Decode(bundleIdText);
        if (bundleId.length !== 32)
            throw new Error('length');
    } catch (e) {
        showError('This address does not name a Kelp bundle.');
        return;
    }

    // The fragment is the key. A browser never sends it anywhere: it is not
    // part of the path, the query, or any header.
    var fragment = location.hash.replace(/^#/, '');
    var fields = {};
    fragment.split('&').forEach(function (part) {
        var cut = part.indexOf('=');
        if (cut > 0)
            fields[part.slice(0, cut)] = part.slice(cut + 1);
    });

    // A passphrase link wraps the secret under a passphrase (§12.3). The
    // gateway can unwrap it in the browser: PBKDF2-HMAC-SHA256 derives a key
    // from the passphrase, and AES-256-GCM decrypts the wrapped secret with
    // the bundle id as authenticated data. The user types the passphrase here;
    // nothing leaves the browser.
    if (fields.p) {
        // Decode the wrapped blob (81 bytes).
        var wrappedBytes;
        try {
            wrappedBytes = G.base64UrlDecode(fields.p);
        } catch (e) {
            showError('This link is damaged: the wrapped secret cannot be read.');
            return;
        }
        if (wrappedBytes.length !== 81) {
            showError('This link is damaged: the wrapped secret has the wrong size.');
            return;
        }
        if (wrappedBytes[0] !== 1) {
            showError('This link uses a passphrase format this gateway does not support.');
            return;
        }
        var iterations = (wrappedBytes[1] << 24) | (wrappedBytes[2] << 16)
                       | (wrappedBytes[3] << 8) | wrappedBytes[4];
        if (iterations < 1 || iterations > 10000000) {
            showError('This link asks for an unreasonable number of key-derivation rounds.');
            return;
        }
        var salt = wrappedBytes.slice(5, 21);
        var nonce = wrappedBytes.slice(21, 33);
        var ciphertext = wrappedBytes.slice(33); // 48 bytes: 32 secret + 16 tag

        // Build a passphrase input in the page.
        var input = document.createElement('input');
        input.type = 'password';
        input.placeholder = 'Passphrase';
        input.style.cssText = 'width:100%;padding:.5rem;margin:.5rem 0;font:inherit;';
        var button = document.createElement('button');
        button.textContent = 'Unlock';
        button.style.cssText = 'margin:.5rem 0;';
        var container = document.createElement('div');
        container.appendChild(input);
        container.appendChild(document.createElement('br'));
        container.appendChild(button);
        statusEl.parentNode.insertBefore(container, statusEl.nextSibling);
        setStatus('This link is protected by a passphrase. Type it here.', '');

        button.addEventListener('click', function () {
            var passphrase = input.value;
            if (!passphrase) {
                showError('Type the passphrase that came with the link.');
                return;
            }
            setStatus('Unlocking…', 'busy');
            button.disabled = true;

            // Derive the key with PBKDF2-HMAC-SHA256.
            var enc = new TextEncoder();
            var passphraseBytes = enc.encode(passphrase);
            window.crypto.subtle.importKey(
                'raw', passphraseBytes, 'PBKDF2', false, ['deriveKey']
            ).then(function (keyMaterial) {
                return window.crypto.subtle.deriveKey(
                    { name: 'PBKDF2', salt: salt, iterations: iterations, hash: 'SHA-256' },
                    keyMaterial,
                    { name: 'AES-GCM', length: 256 },
                    false,
                    ['decrypt']
                );
            }).then(function (aesKey) {
                // Decrypt the wrapped secret. AAD = bundle id.
                return window.crypto.subtle.decrypt(
                    { name: 'AES-GCM', iv: nonce, additionalData: bundleId },
                    aesKey,
                    ciphertext
                );
            }).then(function (plainBuf) {
                var secretBytes = new Uint8Array(plainBuf);
                if (secretBytes.length !== 32) {
                    throw new Error('bad secret length');
                }
                // Base64url-encode the unwrapped secret and replace the
                // passphrase fragment with a key fragment, then reload so the
                // whole bootstrap runs again with the unwrapped key. The
                // passphrase never reaches the URL history beyond this point.
                var b64 = G.base64UrlEncode(Array.from(secretBytes));
                location.hash = '#k=' + b64;
                location.reload();
            }).catch(function (e) {
                showError('That passphrase did not work. Check it and try again.');
                button.disabled = false;
            });
        });

        // Stop here until the user unlocks.
        return;
    }
    if (!fields.k) {
        showError('This link has no key. The part after “#” is what opens the '
                  + 'bundle, and it is missing.');
        return;
    }

    if (!(window.crypto && window.crypto.subtle) || !window.TextDecoder) {
        showError('This browser cannot decrypt here. The gateway needs a secure '
                  + 'connection (HTTPS) with a certificate the browser accepts.');
        return;
    }
    if (!navigator.serviceWorker) {
        // Files can still be listed and downloaded without a worker; only a page
        // needs one. Say so once, then carry on.
        setStatus('This browser cannot serve a page here (no service worker). '
                  + 'Files can still be downloaded.', 'error');
    }

    var secret;
    try {
        secret = G.base64UrlDecode(fields.k);
    } catch (e) {
        showError('That key is not a Kelp key.');
        return;
    }
    if (secret.length !== 32) {
        showError('That key is not a Kelp key.');
        return;
    }

    // --- the walk ----------------------------------------------------------

    var keys = null;
    var tree = null;
    // The link's own metadata — the label the archive is named after, and the
    // page hint — sealed in `q=` under a key derived from the secret. Absent or
    // unreadable is not an error: the archive falls back to its bundle id.
    var locations = null;

    G.prepare(secret).then(function (derived) {
        keys = derived;
        if (!fields.q)
            return null;
        return G.openLocations(secret, bundleId, fields.q).catch(function () { return null; });
    }).then(function (opened) {
        locations = opened;
        // One cheap question first, so "the sender is not online" is said in
        // those words rather than as a failed object.
        return fetch(G.apiBase(bundleId, keys.relayToken) + '/info');
    }).then(function (response) {
        if (response.status === 401) {
            // The token is derived from the link, so this can only mean a relay
            // that does not carry this bundle.
            throw new Error('This relay is not carrying that bundle.');
        }
        if (!response.ok) {
            throw new Error(response.status === 404
                ? 'The sender is not online. Ask them to share again, then reload this page.'
                : 'The relay answered ' + response.status + '.');
        }
        return G.fullTree(bundleId, keys.relayToken, keys.contentKey, fetch,
                          function (done, total) {
            setStatus('Reading the bundle (' + done + ' of ' + total + ')', 'busy');
        });
    }).then(function (walked) {
        tree = walked;
        if (tree.files.length === 0) {
            showError('This bundle is empty.');
            return;
        }
        var entry = G.chooseEntry(tree.files);
        if (entry) {
            return openAsPage(entry);
        }
        showFileList();
    }).catch(function (error) {
        showError(error && error.message ? error.message : String(error));
    });

    // --- a page: the entry document under this URL space --------------------

    function openAsPage(entry) {
        if (!navigator.serviceWorker) {
            // Without a worker, the page cannot be served as a page. The files
            // are still readable, so show them rather than a dead end.
            showFileList();
            return;
        }

        var session = G.pageSession(bundleIdText, keys, tree, entry);

        setStatus('Preparing the page', 'busy');
        return navigator.serviceWorker.register('.kelp/sw.js', { scope: prefix })
            .then(function (registration) { return navigator.serviceWorker.ready; })
            .then(function (registration) {
                registration.active.postMessage(session);
                if (navigator.serviceWorker.controller) {
                    location.reload();
                    return new Promise(function () {});   // the reload replaces us
                }
                return new Promise(function (resolve) {
                    navigator.serviceWorker.addEventListener('controllerchange',
                                                           function () {
                        location.reload();
                    });
                });
            });
    }

    // --- no page: the folder (plan D7, §12.12) -----------------------------
    //
    // A bundle with no page is a folder. One button hands over all of it as one
    // stored ZIP, named after the folder the link names; the per-file buttons
    // stay for the single file someone actually wanted.

    function saveBlob(blob, name) {
        var url = URL.createObjectURL(blob);
        var link = document.createElement('a');
        link.href = url;
        link.download = name;
        document.body.appendChild(link);
        link.click();
        link.remove();
        setTimeout(function () { URL.revokeObjectURL(url); }, 60000);
    }

    function folderArchiveName() {
        var fallback = 'kelp-' + bundleIdText.slice(0, 8);
        var label = locations && locations.name ? String(locations.name) : '';
        var safe = G.zipSafeName(label).replace(/\//g, '_');
        if (!safe)
            return fallback + '.zip';
        return /\.zip$/i.test(safe) ? safe : safe + '.zip';
    }

    function showFileList() {
        var totalBytes = 0;
        tree.files.forEach(function (file) { totalBytes += file.size; });
        var files = tree.files.length;
        var dirs = tree.dirs ? tree.dirs.length : 0;
        var tooMany = files + dirs > G.ZIP_MAX_ENTRIES;
        var tooBig = totalBytes > G.ZIP_MAX_BYTES;
        var fits = !tooMany && !tooBig;

        setStatus('This folder has ' + files + ' file' + (files === 1 ? '' : 's'), '');
        var fetcher = G.makeFetcher(bundleId, keys.relayToken, keys.contentKey, fetch);

        var archiveLabel = 'Download folder (' + files + ' file' + (files === 1 ? '' : 's')
            + ', ' + humanBytes(totalBytes) + ')';
        var all = document.createElement('button');
        all.textContent = archiveLabel;
        all.disabled = !fits;

        var note = document.createElement('p');
        note.className = 'note';
        if (tooMany)
            note.textContent = 'This folder has too many files for one archive. '
                + 'Download files one by one.';
        else if (tooBig)
            note.textContent = 'This folder is too large for one archive (over '
                + humanBytes(G.ZIP_MAX_BYTES) + '). Download files one by one.';

        var bar = document.createElement('div');
        bar.className = 'allbar';
        bar.appendChild(all);
        if (note.textContent)
            bar.appendChild(note);
        filesEl.parentNode.insertBefore(bar, filesEl);

        var buttons = [];
        var building = false;
        var cancelWanted = false;

        function setRowsEnabled(enabled) {
            buttons.forEach(function (button) { button.disabled = !enabled; });
        }

        all.addEventListener('click', function () {
            if (building) {
                cancelWanted = true;
                all.textContent = 'Cancelling…';
                all.disabled = true;
                return;
            }
            if (!fits)
                return;
            building = true;
            cancelWanted = false;
            all.textContent = 'Cancel';
            setRowsEnabled(false);
            setStatus('Preparing the archive', 'busy');
            // One file at a time, chunks in parallel inside a file: the working
            // set is one file, and the archive is only handed over when every
            // byte has been verified and appended.
            G.zipBundle(fetcher, tree.files, tree.dirs || [], {
                cancelled: function () { return cancelWanted; },
                onProgress: function (done, total, bytes, sum) {
                    setStatus('Fetching ' + done + ' of ' + total + ' ('
                              + humanBytes(bytes) + ' of ' + humanBytes(sum) + ')', 'busy');
                }
            }).then(function (zip) {
                var name = folderArchiveName();
                saveBlob(new Blob(zip.parts, { type: 'application/zip' }), name);
                setStatus('Saved ' + name + ' (' + humanBytes(zip.bytes) + ')', '');
            }).catch(function (error) {
                if (error && error.cancelled)
                    setStatus('Cancelled — nothing was saved.', '');
                else
                    showError('Could not build the archive: '
                              + (error && error.message ? error.message : error)
                              + ' Nothing was saved.');
            }).then(function () {
                building = false;
                cancelWanted = false;
                all.textContent = archiveLabel;
                all.disabled = false;
                setRowsEnabled(true);
            });
        });

        tree.files.forEach(function (file) {
            var row = document.createElement('li');

            var name = document.createElement('span');
            name.className = 'name';
            name.textContent = file.path;

            var size = document.createElement('span');
            size.className = 'size';
            size.textContent = humanBytes(file.size);

            var button = document.createElement('button');
            button.textContent = 'Download';
            buttons.push(button);
            button.addEventListener('click', function () {
                button.disabled = true;
                all.disabled = true;
                setStatus('Fetching ' + file.path, 'busy');
                // Every chunk is verified against its id before it is decrypted,
                // so a lying relay produces a failed download, not a wrong file.
                Promise.all(file.chunks.map(fetcher)).then(function (parts) {
                    saveBlob(new Blob(parts, { type: G.mimeFor(file) }),
                             file.path.split('/').pop() || 'file');
                    setStatus('Saved ' + file.path, '');
                }).catch(function (error) {
                    showError('Could not fetch ' + file.path + ': '
                              + (error && error.message ? error.message : error));
                }).then(function () {
                    button.disabled = false;
                    all.disabled = !fits;
                });
            });

            row.appendChild(name);
            row.appendChild(size);
            row.appendChild(button);
            filesEl.appendChild(row);
        });
    }
}());
