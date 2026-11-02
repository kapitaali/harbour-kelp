# AGENTS.md — harbour-kelp

Instructions for any agent working in this repository. Read this first, then `README.md`, then `META-INF/BUILD.md` and `META-INF/kelp-design.md`.

## 1. What this repo is

Kelp is a native Sailfish OS app for secure, minimal peer-to-peer file sharing. It seals files into encrypted, content-addressed bundles served over the LAN (and optionally via a separate relay program), with `kelp://` capability links. The app is qmake/Sailfish, built with sfdk; `libkelp` is the C++ crypto/protocol core with unit tests.

## 2. The one command that decides whether you are done

```
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-all-tests.sh"
```

Builds and runs every test suite inside the build engine. A red gate is never "pre-existing" — find out what changed. Also run the gateway suite on the host:

```
sh relay/gateway/test/run-gateway-test.sh   # ALL PASS (71 checks)
```

## 3. Layout

```
src/            C++ backend (kelp, bundles, store, fetch, server, preview, policy, settings, shares, handoff, qr, device state)
src/tests/      test suites + run-all-tests.sh
qml/            Silica UI pages
libkelp/        crypto/protocol core + tests
relay/          relay program + browser gateway (separate programs)
rpm/            Harbour spec (dev-extras %define lives here)
META-INF/       design, build, progress, submission docs
icons/          app icons
```

## 4. Laws. Breaking one is a bug even if the tests pass.

1. **Clean before switching targets.** `sfdk build` compiles in place; switching arch or defines without `rm -f harbour-kelp *.o moc_*.cpp *.moc Makefile .qmake.stash` (or `make distclean` in the engine) links wrong objects. The dev-extras bundle is a `-D` define and `make` cannot see a `-D` change — flip it with a clean.
2. **Dev vs release.** `%define kelp_dev_extras 0` for release; dev builds set `KELP_DEV_EXTRAS=1`. Release never logs a `kelp://` link and never offers the switch. Verify with `grep -c KELP_DEV_BUILD Makefile` (0) and `grep -c 'kelp://' <log>` (0).
3. **Harbour rules.** `Qt.labs.folderlistmodel 2.1` for folder picking (no `Nemo.FileManager`); `Exec=harbour-kelp %u`; no `%post`/`%postun` scriptlets; `License: GPL-3.0-only` with repo-root `LICENSE` (not `/usr/share/licenses`); minimal `[X-Sailjail]` permissions.
4. **No secrets in committed files.** Never record `kelp://` links, fragments, keys, QR payloads, or phone LAN addresses in docs, reports, screenshots, or chat.
5. **Tests run in the engine.** Target Qt/OpenSSL; suites run out-of-source in `.build` dirs cleaned first. Run both bundle modes (dev + release) for the two suites whose default follows the build.
6. **Fixtures/generated files are regenerated, not hand-edited.** Gateway fixture is a build artifact (`.build/fixture.json`, gitignored).

## 5. Verification culture

- Every claim about behaviour is reproducible by a command in this repo; state target, SDK version, and build mode.
- Negative results and failures get the same prominence as successes.
- Prefer measured numbers over adjectives. No marketing language.

## 6. Recording work

- Progress goes in `META-INF/PROGRESS-YYYYMMDD.md` (gitignored); design stays in `META-INF/kelp-design.md` and `META-INF/kelp-swarm-wire-spec.md`.
- Add a short pointer in the design doc to the newest progress note.
- Never write a capability secret into any of these files (redact link fragments).

## 7. Git and secrets

- Never push. The owner pushes.
- Never commit credentials, tokens, `.env` files, unredacted traces, or build artifacts (`RPMS/`, `*.o`, `moc_*.cpp`, `Makefile`, `.qmake.stash`).
- Commits are attributed `kapitaali <teppo@kapitaali.com>`.
- Do not stage another agent's work in progress — check `git status` and `git log --oneline -5` first.

## 8. Test commands

```
# all suites, one arch
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-all-tests.sh"
# gateway (host, needs sfdk + node)
sh relay/gateway/test/run-gateway-test.sh
# QML smoke (compiles every page)
src/tests/run-qml-smoke.sh
# Harbour validator
sfdk -c device= -c target=SailfishOS-5.1.0.11-aarch64 check -s harbour RPMS/SailfishOS-5.1.0.11-aarch64/harbour-kelp-0.1.0-1.aarch64.rpm
```

The suites print authoritative check counts; do not hardcode them in docs.

## 9. Current state

- App shares bundles on the LAN only (no relay path in the app); `kelp-relay` and its browser gateway are separate programs with their own tests.
- Webpage shares (preview in browser, nothing stored) are implemented; receiving half of webpage shares and swarm (peer helping) are not.
- Passphrase links (`#p=`) are implemented.
- Build id falls back to a UTC timestamp in the engine (no git there); identify builds by their log line.