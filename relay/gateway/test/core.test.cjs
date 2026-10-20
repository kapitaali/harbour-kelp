/*
 * The gateway's JavaScript core, checked against real seals (design §5.2,
 * C4b-plan.md).
 *
 * The fixture is written by makefixture, which seals three bundles with libkelp
 * and records libkelp's own answers about them: the object envelope, the two
 * HKDF derivations, and the §5.3 page convention. This file is the other half of
 * that promise — it does the same work in JavaScript and must reach the same
 * answers, byte for byte, or a recipient's browser would disagree with the app
 * about a link they were both handed.
 *
 * Run: node core.test.cjs <fixture.json>
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync } = require('child_process');

// core.js is written for a browser and a worker: it installs itself on `self`
// when there is one. In Node there is not, so it lands on module.exports.
const loaded = require(path.join(__dirname, '..', 'core.js'));
const G = loaded.KelpGateway || globalThis.KelpGateway;

if (!G) {
    console.error('FAIL core.js did not install itself');
    process.exit(1);
}

let failures = 0;
let checks = 0;

function check(name, ok, detail) {
    ++checks;
    if (ok) {
        console.log('PASS ' + name);
    } else {
        ++failures;
        console.log('FAIL ' + name + (detail === undefined ? '' : ' — ' + detail));
    }
}

function bytesOfBase64Url(text) {
    return Buffer.from(text, 'base64url');
}

function hex(bytes) {
    return Buffer.from(bytes).toString('hex');
}

function join(parts) {
    const total = parts.reduce((sum, part) => sum + part.length, 0);
    const whole = new Uint8Array(total);
    let at = 0;
    parts.forEach((part) => {
        whole.set(part, at);
        at += part.length;
    });
    return whole;
}

// What the browser's `fetch` would do, answered out of the fixture: the URL
// shape is the relay's own, so the JavaScript builds and parses the real thing.
function shim(bundle, options) {
    const objects = new Map();
    bundle.objects.forEach((object) => {
        objects.set(object.id, Buffer.from(object.bytes, 'base64url'));
    });
    const asked = [];
    const doFetch = async (url) => {
        const match = /^\/v1\/relay\/([A-Za-z0-9_-]+)\/bundle\/([a-z2-7]+)\/object\/([a-z2-7]+)$/
            .exec(url);
        if (!match) {
            asked.push({ url: url, status: 0 });
            return { ok: false, status: 400, arrayBuffer: async () => new ArrayBuffer(0) };
        }
        asked.push({ url: url, token: match[1], id: match[3] });
        if (!objects.has(match[3]))
            return { ok: false, status: 404, arrayBuffer: async () => new ArrayBuffer(0) };
        let bytes = objects.get(match[3]);
        if (options && options.corrupt === match[3]) {
            bytes = Buffer.from(bytes);
            bytes[0] = bytes[0] ^ 0xff;                 // the relay lied
        }
        return { ok: true, status: 200, arrayBuffer: async () => bytes };
    };
    return { doFetch: doFetch, asked: asked, objects: objects };
}

function sameNames(a, b) {
    if (a.length !== b.length)
        return false;
    for (let i = 0; i < a.length; ++i) {
        if (a[i] !== b[i])
            return false;
    }
    return true;
}

// An independent ZIP reader for the tests: it walks the central directory the
// way an extractor does, and returns every entry with its bytes. Nothing here
// shares code with the writer, so the two cannot agree on a mistake.
function readZip(buffer) {
    const eocd = buffer.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
    if (eocd < 0)
        throw new Error('no end-of-central-directory record');
    if (buffer.readUInt16LE(eocd + 4) !== 0 || buffer.readUInt16LE(eocd + 6) !== 0)
        throw new Error('a multi-disk archive');
    const count = buffer.readUInt16LE(eocd + 10);
    const start = buffer.readUInt32LE(eocd + 16);

    const entries = [];
    let at = start;
    for (let i = 0; i < count; ++i) {
        if (buffer.readUInt32LE(at) !== 0x02014B50)
            throw new Error('no central directory header at ' + at);
        const flags = buffer.readUInt16LE(at + 8);
        const method = buffer.readUInt16LE(at + 10);
        const crc = buffer.readUInt32LE(at + 16);
        const compressed = buffer.readUInt32LE(at + 20);
        const uncompressed = buffer.readUInt32LE(at + 24);
        const nameLength = buffer.readUInt16LE(at + 28);
        const extraLength = buffer.readUInt16LE(at + 30);
        const commentLength = buffer.readUInt16LE(at + 32);
        const local = buffer.readUInt32LE(at + 42);
        const name = buffer.toString('utf8', at + 46, at + 46 + nameLength);
        if (buffer.readUInt32LE(local) !== 0x04034B50)
            throw new Error('no local header for ' + name);
        const localNameLength = buffer.readUInt16LE(local + 26);
        const localExtra = buffer.readUInt16LE(local + 28);
        const dataAt = local + 30 + localNameLength + localExtra;
        entries.push({
            name: name,
            localName: buffer.toString('utf8', local + 30, local + 30 + localNameLength),
            flags: flags,
            method: method,
            crc: crc,
            compressed: compressed,
            uncompressed: uncompressed,
            data: buffer.subarray(dataAt, dataAt + compressed)
        });
        at += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
}

async function run() {
    const fixturePath = process.argv[2];
    if (!fixturePath) {
        console.error('usage: node core.test.cjs <fixture.json>');
        process.exit(2);
    }
    const fixture = JSON.parse(fs.readFileSync(fixturePath, 'utf8'));

    for (const bundle of fixture.bundles) {
        const secret = bytesOfBase64Url(bundle.secret);
        const where = '[' + bundle.name + '] ';

        // 1. The two derivations, against libkelp's.
        const keys = await G.prepare(secret);
        check(where + 'relay_token matches libkelp',
              G.base64UrlEncode(keys.relayToken) === bundle.token,
              G.base64UrlEncode(keys.relayToken) + ' != ' + bundle.token);
        check(where + 'the content key is 32 bytes', keys.contentKey.length === 32);

        // 2. The bundle id in the link decodes to the root manifest's address.
        const rootId = G.base32Decode(bundle.bundle);
        check(where + 'the bundle id is a 32-byte address', rootId.length === 32);
        check(where + 'the bundle id is the root manifest',
              G.base32Encode(rootId) === bundle.bundle);

        // 3. The tree, walked exactly as the browser walks it.
        const fake = shim(bundle);
        const tree = await G.fullTree(rootId, keys.relayToken, keys.contentKey,
                                      fake.doFetch);
        const walked = tree.files.map((file) => file.path);
        const expected = bundle.files.map((file) => file.path);
        check(where + 'the walk finds every file',
              sameNames(walked, expected), walked.join(', ') + ' vs ' + expected.join(', '));

        // 4. Sizes and MIME types: the manifest's own, and libkelp's table.
        let sizesAgree = true;
        let mimesAgree = true;
        bundle.files.forEach((file) => {
            const found = tree.files.find((candidate) => candidate.path === file.path);
            if (!found)
                return;
            if (Number(found.size) !== file.size)
                sizesAgree = false;
            if (G.mimeFor(found) !== file.mime)
                mimesAgree = false;
        });
        check(where + 'every size is the manifest\'s', sizesAgree);
        check(where + 'every MIME type is libkelp\'s', mimesAgree);

        // 5. Which file is the page, and the URL space it makes (§5.3).
        const entry = G.chooseEntry(tree.files);
        check(where + 'the entry document is libkelp\'s',
              (entry ? entry.path : '') === bundle.entry,
              (entry ? entry.path : '(none)') + ' vs ' + (bundle.entry || '(none)'));
        const root = entry ? G.webRootOf(entry) : '';
        check(where + 'the web root is libkelp\'s',
              root === (bundle.root === '' ? '' : bundle.root + '/'),
              root + ' vs ' + bundle.root);
        const candidates = tree.files
            .filter((file) => G.isHtml(file))
            .map((file) => file.path);
        check(where + 'the HTML candidates are libkelp\'s',
              sameNames(candidates, bundle.candidates),
              candidates.join(', ') + ' vs ' + bundle.candidates.join(', '));

        // 6. Every file, fetched chunk by chunk and decrypted: the digest has to
        //    be the one libkelp computed over the plaintext it sealed. This is
        //    also what proves a multi-chunk file comes out in the right order.
        const fetcher = G.makeFetcher(rootId, keys.relayToken, keys.contentKey, fake.doFetch);
        let digestsAgree = true;
        for (const file of tree.files) {
            const parts = await Promise.all(file.chunks.map(fetcher));
            const total = parts.reduce((sum, part) => sum + part.length, 0);
            const whole = new Uint8Array(total);
            let at = 0;
            parts.forEach((part) => {
                whole.set(part, at);
                at += part.length;
            });
            const digest = G.base64UrlEncode(await G.sha256(whole));
            const want = bundle.files.find((candidate) => candidate.path === file.path);
            if (!want || digest !== want.sha256) {
                digestsAgree = false;
                console.log('       ' + file.path + ': ' + digest + ' != ' +
                            (want ? want.sha256 : '(missing)'));
            }
        }
        check(where + 'every file decrypts to libkelp\'s plaintext', digestsAgree);

        // The entry document is the sender's own page, not a stray object.
        if (entry) {
            const parts = await Promise.all(entry.chunks.map(fetcher));
            check(where + 'the entry document reads as HTML',
                  Buffer.from(parts[0]).toString('utf8').indexOf('<!doctype html>') === 0);
        }

        // 7. The URL space the worker answers in: directories become index.html.
        if (entry) {
            const space = {};
            const files = G.urlSpace(tree.files, root);
            Object.keys(files).forEach((url) => { space[url] = 1; });
            const entryUrl = entry.path.slice(root.length);
            check(where + 'the entry answers at /',
                  G.resolvePath(space, entryUrl, '') === entryUrl);
            check(where + 'the entry answers at / as well',
                  G.resolvePath(space, entryUrl, '/') === entryUrl);
            if (bundle.name === 'page') {
                check(where + 'a file answers at its own path',
                      G.resolvePath(space, entryUrl, 'style.css') === 'style.css');
                check(where + 'a directory with an index finds it without a slash',
                      G.resolvePath(space, entryUrl, 'sub/inner') === 'sub/inner/index.html');
                check(where + 'a directory with an index finds it with a slash',
                      G.resolvePath(space, entryUrl, 'sub/inner/') === 'sub/inner/index.html');
                check(where + 'a directory with no index of its own resolves to nothing',
                      G.resolvePath(space, entryUrl, 'sub') === null
                          && G.resolvePath(space, entryUrl, 'sub/') === null);
                check(where + 'a path that is not there resolves to nothing',
                      G.resolvePath(space, entryUrl, 'nothing/') === null);
            }
            if (bundle.name === 'nested') {
                // The web root is sub/, so what is above it is not in the space
                // the worker answers in — it cannot be reached by URL at all.
                check(where + 'files above the web root are not in the space',
                      space['../readme.txt'] === undefined
                          && space['deep/deeper/index.html'] === undefined
                          && G.resolvePath(space, entryUrl, 'deep/deeper/') === null);
            }
        } else {
            check(where + 'a bundle with no page has no entry', entry === null);
        }

        // 8. A relay that substitutes an object is caught before decryption.
        let caught = false;
        try {
            const liar = shim(bundle, { corrupt: bundle.objects[0].id });
            const liarFetcher = G.makeFetcher(rootId, keys.relayToken, keys.contentKey,
                                              liar.doFetch);
            await liarFetcher(G.base32Decode(bundle.objects[0].id));
        } catch (error) {
            caught = /does not match its address/.test(error.message);
        }
        check(where + 'a substituted object is refused', caught);

        // 9. A foreign key does not open the objects, and says so.
        let refused = false;
        try {
            const wrong = await G.contentKey(Buffer.alloc(32, 0x5a));
            const wrongFetcher = G.makeFetcher(rootId, keys.relayToken, wrong, fake.doFetch);
            const first = tree.files[0];
            await wrongFetcher(first.chunks[0]);
        } catch (error) {
            refused = /did not open|does not match/.test(error.message);
        }
        check(where + 'a foreign key opens nothing', refused);

        // 10. The relay's own paths: what the loader asks for by name.
        check(where + 'the object URL is the relay\'s',
              G.objectUrl(rootId, keys.relayToken, rootId) ===
                  '/v1/relay/' + bundle.token + '/bundle/' + bundle.bundle +
                  '/object/' + bundle.bundle);
        check(where + 'the info URL is the relay\'s',
              G.apiBase(rootId, keys.relayToken) ===
                  '/v1/relay/' + bundle.token + '/bundle/' + bundle.bundle);

        // 11. The handover from the bootstrap to the worker — the two bugs a real
        //     browser found. Both keys must cross as bytes: the URL builder
        //     encodes the token itself, so base64url text here was one encoding
        //     too many and the relay answered 401. And every file must be named
        //     by its chunks, in order: naming it by its manifest (file.hash)
        //     served the manifest as the page, which rendered blank.
        if (entry) {
            const session = G.pageSession(bundle.bundle, keys, tree, entry);
            check(where + 'the session carries the relay token as bytes',
                  session.token instanceof Uint8Array && session.token.length === 32,
                  'token is a ' + (session.token && session.token.constructor
                      ? session.token.constructor.name : typeof session.token));
            check(where + 'the session carries the content key as bytes',
                  session.key instanceof Uint8Array && session.key.length === 32);

            const webRoot = G.webRootOf(entry);
            const sessionFile = session.files[session.entry];
            const entryFile = tree.files.find((file) => file.path === entry.path);
            check(where + 'the entry is named by its chunks, not its manifest',
                  sessionFile && sameNames(sessionFile.chunks,
                                           entryFile.chunks.map(G.base32Encode))
                      && sessionFile.chunks.indexOf(G.base32Encode(entryFile.hash)) < 0,
                  sessionFile ? sessionFile.chunks.join(',') : '(no entry file)');

            // Serve every URL the way sw.js does: all chunks, in order, then the
            // digest libkelp computed over the plaintext it sealed.
            let sessionAgrees = true;
            for (const url of Object.keys(session.files)) {
                const file = session.files[url];
                const want = bundle.files.find((candidate) => candidate.path === webRoot + url);
                const workerShim = shim(bundle);
                const fetcher = G.sessionFetcher(session, workerShim.doFetch);
                const parts = await Promise.all(file.chunks.map(
                    (id) => fetcher(G.base32Decode(id))));
                const digest = G.base64UrlEncode(await G.sha256(join(parts)));
                if (!want || digest !== want.sha256) {
                    sessionAgrees = false;
                    console.log('       ' + url + ': ' + digest + ' != ' +
                                (want ? want.sha256 : '(no such file)'));
                }
            }
            check(where + 'a page\'s files are served as their whole content',
                  sessionAgrees);

            // The URL a worker builds is the URL the loader built: one token.
            const workerShim = shim(bundle);
            const chunk = G.base32Decode(session.files[session.entry].chunks[0]);
            await G.sessionFetcher(session, workerShim.doFetch)(chunk);
            check(where + 'a worker fetch puts the token in the URL once',
                  workerShim.asked.length === 1
                      && workerShim.asked[0].url === G.objectUrl(rootId, keys.relayToken, chunk)
                      && workerShim.asked[0].token === bundle.token,
                  workerShim.asked.length ? workerShim.asked[0].url : '(no fetch happened)');
        }

        // 12. The link's own metadata: the archive is named after the folder.
        const qMatch = /[?&]q=([^#&]+)/.exec(bundle.link || '');
        check(where + 'the link carries its sealed q= block', !!qMatch);
        if (qMatch) {
            const opened = await G.openLocations(secret, rootId, qMatch[1]);
            check(where + 'the link label opens, and names the folder',
                  opened.name === bundle.name, opened.name + ' vs ' + bundle.name);
            check(where + 'the link page hint opens',
                  opened.page === (bundle.entry !== ''), String(opened.page));
            let bound = false;
            try {
                await G.openLocations(secret, Buffer.alloc(32, 0x11), qMatch[1]);
            } catch (error) {
                bound = true;
            }
            check(where + 'the q= block is bound to its bundle id', bound);
        }

        // 13. The archive: the whole folder, built by the function the page
        //     calls, then checked by an independent reader and by unzip itself.
        const zip = await G.zipBundle(fetcher, tree.files, tree.dirs, {});
        const zipBytes = Buffer.from(join(zip.parts));
        const zipPath = path.join(__dirname, '.build', 'zip-' + bundle.name + '.zip');
        fs.writeFileSync(zipPath, zipBytes);

        const entries = readZip(zipBytes);
        check(where + 'the archive holds every file and folder',
              entries.length === tree.files.length + tree.dirs.length,
              entries.length + ' entries for ' + tree.files.length + ' files and '
                  + tree.dirs.length + ' folders');
        check(where + 'every entry is stored, UTF-8, with no data descriptor',
              entries.every((e) => e.method === 0 && (e.flags & 0x0800)
                                  && (e.flags & 0x0008) === 0));
        check(where + 'every entry\'s name is the name in its local header',
              entries.every((e) => e.name === e.localName));

        const names = G.zipNames(tree.files, tree.dirs);
        const fileIndex = new Map();
        tree.files.forEach((file, i) => fileIndex.set(names.files[i], file));
        let filesAgree = true;
        let foldersAgree = true;
        entries.forEach((entry) => {
            if (entry.name.slice(-1) === '/') {
                if (entry.uncompressed !== 0 || entry.crc !== 0
                    || names.dirs.indexOf(entry.name.slice(0, -1)) < 0)
                    foldersAgree = false;
                return;
            }
            const file = fileIndex.get(entry.name);
            const want = file
                ? bundle.files.find((candidate) => candidate.path === file.path)
                : null;
            const digest = G.base64UrlEncode(
                crypto.createHash('sha256').update(entry.data).digest());
            if (!want || entry.uncompressed !== want.size
                || entry.crc !== G.crc32(entry.data) || digest !== want.sha256) {
                filesAgree = false;
                console.log('       ' + entry.name + ': '
                            + (want ? 'mismatch' : 'not in the bundle'));
            }
        });
        check(where + 'every file comes out as libkelp\'s plaintext', filesAgree);
        check(where + 'every folder is an entry of its own', foldersAgree);
        check(where + 'the writer\'s byte count is the folder\'s',
              zip.bytes === bundle.files.reduce((sum, f) => sum + f.size, 0));

        // The real tool's verdict, not only our own reader's.
        let unzipSays = '';
        try {
            unzipSays = execFileSync('unzip', ['-t', zipPath], { encoding: 'utf8' });
        } catch (error) {
            unzipSays = String(error.stdout || '') + String(error.stderr || '');
        }
        check(where + 'unzip -t accepts the archive',
              /No errors detected/.test(unzipSays),
              unzipSays.trim().split('\n').slice(-1)[0]);
        const listed = execFileSync('unzip', ['-Z1', zipPath], { encoding: 'utf8' })
            .trim().split('\n');
        check(where + 'unzip lists exactly the names the writer meant',
              sameNames(listed, entries.map((e) => e.name)),
              listed.join(', ') + ' vs ' + entries.map((e) => e.name).join(', '));

        // A lying relay aborts the archive: no blob, no file.
        let zipRefused = false;
        try {
            const liar = shim(bundle, { corrupt: G.base32Encode(tree.files[0].chunks[0]) });
            const liarFetcher = G.makeFetcher(rootId, keys.relayToken, keys.contentKey,
                                              liar.doFetch);
            await G.zipBundle(liarFetcher, tree.files, tree.dirs, {});
        } catch (error) {
            zipRefused = /does not match its address|did not open/.test(error.message);
        }
        check(where + 'a lying relay builds no archive', zipRefused);
    }

    // 14. CRC-32: the values every ZIP reader must agree with.
    check('crc32 of nothing is 0', G.crc32(new Uint8Array(0)) === 0);
    check('crc32 of "123456789" is 0xCBF43926',
          G.crc32(Buffer.from('123456789')) === 0xCBF43926,
          '0x' + G.crc32(Buffer.from('123456789')).toString(16));
    check('crc32 of the pangram is 0x414FA339',
          G.crc32(Buffer.from('The quick brown fox jumps over the lazy dog')) === 0x414FA339);

    // 15. Names: every way a sender's path could leave the folder, or collide.
    const safeCases = [
        ['plain.txt', 'plain.txt'],
        ['sub/plain.txt', 'sub/plain.txt'],
        ['/etc/passwd', 'etc/passwd'],
        ['../../x', '_/_/x'],
        ['a\\b.txt', 'a_b.txt'],
        ['..', '_'],
        ['.', '_'],
        ['trail. ', 'trail'],
        ['C:evil', '_C:evil'],
        ['a//b', 'a/b'],
        ['', '']
    ];
    let safeAgree = true;
    safeCases.forEach(([input, want]) => {
        if (G.zipSafeName(input) !== want) {
            safeAgree = false;
            console.log('       ' + JSON.stringify(input) + ' -> '
                        + JSON.stringify(G.zipSafeName(input)) + ', wanted '
                        + JSON.stringify(want));
        }
    });
    check('every unsafe path is neutralised', safeAgree);

    const duplicateNames = G.zipNames(
        [{path: 'A.txt'}, {path: 'a.txt'}, {path: 'dir/x'}, {path: 'dir/X'}],
        ['dir', 'DIR']);
    check('names that collide on a case-insensitive disk are separated',
          sameNames(duplicateNames.dirs, ['dir', 'DIR (2)'])
              && sameNames(duplicateNames.files,
                           ['A.txt', 'a (2).txt', 'dir/x', 'dir/X (2)']),
          duplicateNames.dirs.join(',') + ' | ' + duplicateNames.files.join(','));

    // 16. The caps and the stop: no fetch, no archive.
    let bigFetched = false;
    let bigRefused = false;
    try {
        await G.zipBundle(function () {
            bigFetched = true;
            return Promise.resolve(new Uint8Array(0));
        }, [{path: 'x', size: G.ZIP_MAX_BYTES + 1, chunks: []}], [], {});
    } catch (error) {
        bigRefused = true;
    }
    check('a folder over the byte cap is refused before fetching', bigRefused && !bigFetched);

    let manyRefused = false;
    try {
        await G.zipBundle(function () {
            return Promise.resolve(new Uint8Array(0));
        }, new Array(G.ZIP_MAX_ENTRIES + 1).fill({path: 'x', size: 0, chunks: []}), [], {});
    } catch (error) {
        manyRefused = true;
    }
    check('a folder over the entry cap is refused', manyRefused);

    let cancelled = false;
    let cancelFetched = false;
    try {
        await G.zipBundle(function () {
            cancelFetched = true;
            return Promise.resolve(new Uint8Array(3));
        }, [{path: 'a', size: 3, chunks: [new Uint8Array(1)]}], [],
        {cancelled: function () { return true; }});
    } catch (error) {
        cancelled = error.cancelled === true;
    }
    check('cancelling stops before the first file, with no archive',
          cancelled && !cancelFetched);

    let shortRefused = false;
    try {
        await G.zipBundle(function () {
            return Promise.resolve(new Uint8Array(3));
        }, [{path: 'a', size: 4, chunks: [new Uint8Array(1)]}], [], {});
    } catch (error) {
        shortRefused = /do not match/.test(error.message);
    }
    check('a file whose bytes disagree with its manifest aborts', shortRefused);

    // 11. The byte-level helpers, which everything above rests on.
    const roundTrip = G.base32Decode(G.base32Encode(G.base32Decode(fixture.bundles[0].bundle)));
    check('base32 round-trips a 32-byte address', roundTrip.length === 32);
    let rejected = false;
    try {
        G.base32Decode('not!base32');
    } catch (error) {
        rejected = true;
    }
    check('base32 refuses a character outside its alphabet', rejected);
    check('base32 refuses a wrong length',
          (function () { try { G.base32Decode('abc'); return false; } catch (e) { return true; } })());

    console.log('');
    console.log(failures === 0
        ? 'ALL PASS (' + checks + ' checks)'
        : 'FAILURES (' + failures + ' of ' + checks + ' checks)');
    process.exit(failures === 0 ? 0 : 1);
}

run().catch((error) => {
    console.error('FAIL the test itself threw: ' + (error && error.stack ? error.stack : error));
    process.exit(1);
});
