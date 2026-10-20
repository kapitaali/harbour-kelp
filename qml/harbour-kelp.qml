import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.KeepAlive 1.2
import "pages"
import "cover"

ApplicationWindow {
    id: kelp

    /*
     * The bundle list.
     *
     * A row appears only once a bundle has really been sealed into the object
     * store (see the Add page), so `link`, `bundleId` and `secret` are the real
     * thing and the link is the only copy of the key. The rows are saved to disk
     * (KelpShares) and restored at startup: the address captured in a link is
     * NOT stored, though, so restoring re-serves on a fresh address and the row
     * carries the new link.
     */
    // "Share in background", "Wi-Fi only" and "Allow peer swarm" are persisted
    // preferences now (KelpSettings), not QML state: a switch the user set and
    // the value the app acts on are the same thing, and they survive a restart.
    // Sailfish does not freeze a backgrounded app — the cover view is a live
    // process — so serving already continues while the cover is up; the
    // KeepAlive below is the part that asks the system not to stop us under
    // memory or power pressure. Closing Kelp outright still ends serving: a
    // Harbour app cannot ship an always-on service (that would be a systemd user
    // service, OpenRepos territory — design §12.8). What may be served at all is
    // decided by the serving policy (src/kelppolicy.h, design §12.8): Wi-Fi only
    // means never over mobile data, and a low battery stops serving until it is
    // charging again.

    // Where bundles are published for recipients off the local network. Empty by
    // default: a share is LAN-only until a relay is named, so nothing is sent
    // anywhere the user did not ask for. "host:port" — see README.
    property string relayUrl: ""
    onRelayUrlChanged: applyRelays()

    // A plain function, not just the property-change handler: a signal handler
    // is not callable by name in QML, so Component.onCompleted cannot invoke it
    // directly.
    function applyRelays() {
        var relays = relayUrl.length > 0 ? [relayUrl] : []
        kelpLog.info("Kelp: relays = " + relays)
        kelpBundles.setRelays(relays)
    }

    // The one way into the Open-a-link page: a link the user pasted, one given at
    // startup, or one another app just handed over (src/kelphandoff.cpp calls this
    // by name). Prefill only — nothing is fetched until the page's own button is
    // pressed, so an activation is never consent to fetch (scheme-plan.md).
    function openLink(link) {
        if (!link || link.length === 0)
            return
        // A second link while the page is showing replaces it instead of stacking
        // a page: onLinkChanged resets the passphrase state there. The page names
        // itself, so this does not depend on what push() returns.
        var page = pageStack.currentPage
        if (page && page.objectName === "openLinkPage") {
            page.link = link
            return
        }
        pageStack.push(Qt.resolvedUrl("pages/OpenLinkPage.qml"), { link: link })
    }

    function addBundle(info) {
        kelpLog.info("addBundle: name=\"" + info.name + "\" kind=" + info.kind
                     + " files=" + info.files + " size=" + info.size
                     + " path=" + info.path + " objects=" + info.objects
                     + " lan=" + info.lan + " swarm=" + (info.swarm === true)
                     + " page=" + (info.page === true))
        bundlesModel.append({
            "name": info.name,
            "kind": info.kind,
            "files": info.files,
            "size": info.size,
            "bytes": info.bytes,
            "path": info.path,
            "swarm": info.swarm === true,
            "page": info.page === true,
            // "Seeding" only means something when there is an address to serve
            // on: that is the difference between a link that works and one that
            // does not. The role is `shareState`, not `state`: a delegate is an
            // Item, and Item already has a `state` property of its own, which
            // shadows the role and is always "" — so a row read that way never
            // said "Seeding" and a paused share never offered to resume.
            "shareState": (info.lan && info.lan.length > 0) ? "seeding" : "offline",
            "lan": info.lan ? info.lan : "",
            "link": info.link,
            "bundleId": info.bundleId,
            "secret": info.secret,
            // The wrapped secret of a passphrase share (design §12.3), and empty
            // for every other share. It is what lets a restart mint the same
            // passphrase link again without anyone re-typing the passphrase.
            "passBlob": info.passBlob ? info.passBlob : "",
            "objects": info.objects
        })
        kelpLog.info("addBundle: bundlesModel.count is now " + bundlesModel.count)
        saveShares()
        // A share made while the policy says stop is stopped too: `shareState`
        // "seeding" means "being served", and this is the one place a row
        // appears without going through a resume.
        applyServingPolicy()
    }

    // Writes the row fields the app must remember across a restart. The link
    // itself is not saved: it embeds an address that goes stale the moment the
    // app exits, so it is reminted by restoreShares() instead. `paused` IS saved,
    // though — a share the user silenced should stay silent, not come back
    // serving itself at the next launch. `page` is saved for the same reason: a
    // webpage share is a webpage share however many times the app restarts
    // (design §5.5), and the flag is what restoreShares() feeds back to
    // republish().
    function saveShares() {
        var rows = []
        for (var i = 0; i < bundlesModel.count; ++i) {
            var row = bundlesModel.get(i)
            rows.push({
                "name": row.name,
                "kind": row.kind,
                "files": row.files,
                "size": row.size,
                "bytes": row.bytes,
                "path": row.path,
                "swarm": row.swarm,
                "page": row.page === true,
                "objects": row.objects,
                "paused": row.shareState === "paused",
                "bundleId": row.bundleId,
                "secret": row.secret,
                "passBlob": row.passBlob ? row.passBlob : ""
            })
        }
        kelpLog.info("saveShares: writing " + rows.length + " row(s)")
        kelpShares.save(rows)
        sharesRevision++     // a row may have changed in place; pages re-read it
        updateShareCounters()
    }

    // Silences a share without forgetting it: stop serving (LAN and relay), keep
    // the row and the objects. The address in the link is now dead, so the link
    // is dropped with it; resuming mints a fresh one. Returns false if there is
    // nothing at that row or it is already paused.
    function pauseShare(index) {
        if (!bundlesModel || index < 0 || index >= bundlesModel.count)
            return false
        var row = bundlesModel.get(index)
        if (row.shareState === "paused")
            return false
        kelpLog.info("pauseShare: pausing \"" + row.name + "\"")
        kelpBundles.pause(row.bundleId)
        bundlesModel.setProperty(index, "shareState", "paused")
        bundlesModel.setProperty(index, "lan", "")
        bundlesModel.setProperty(index, "link", "")
        saveShares()
        return true
    }

    // Serves a paused share again, exactly as a restart does: republish() mints a
    // fresh link because the LAN port is ephemeral. Returns false if the row is
    // not paused or the objects can no longer be read.
    function resumeShare(index) {
        if (!bundlesModel || index < 0 || index >= bundlesModel.count)
            return false
        var row = bundlesModel.get(index)
        if (row.shareState !== "paused")
            return false
        var published = kelpBundles.republish(row.bundleId, row.secret, row.name,
                                              row.objects, row.files, row.bytes,
                                              row.page === true,
                                              row.passBlob ? row.passBlob : "")
        if (!published.ok) {
            kelpLog.warn("resumeShare: cannot resume \"" + row.name + "\": " + published.error)
            return false
        }
        bundlesModel.setProperty(index, "shareState",
                                 (published.lan && published.lan.length > 0) ? "seeding" : "offline")
        bundlesModel.setProperty(index, "lan", published.lan ? published.lan : "")
        bundlesModel.setProperty(index, "link", published.link)
        saveShares()
        // A resume the policy does not allow is stopped again at once and
        // remembered, so it comes back with the rest when the reason goes away;
        // the row keeps the link this moment minted.
        applyServingPolicy()
        return true
    }

    // Derived counters the cover binds to. They are maintained properties rather
    // than functions the cover calls, because a binding that walks the model with
    // get() re-enters the model while it evaluates and QML reports that as a
    // binding loop (seen live on the cover, §3.16). Every row change funnels
    // through saveShares() or restoreShares(), which is where they are refreshed.
    property int servedShares: 0
    property int pausedShares: 0
    // Shares the serving policy stopped (design §12.8). Counted separately from
    // a user pause, because the cover must not offer to undo something the user
    // did not do and that undoes itself.
    property int blockedShares: 0

    function updateShareCounters() {
        var served = 0
        var paused = 0
        var blocked = 0
        for (var i = 0; i < bundlesModel.count; ++i) {
            var row = bundlesModel.get(i)
            if (row.lan.length > 0) ++served
            if (row.shareState === "paused") ++paused
            if (row.shareState === "blocked") ++blocked
        }
        servedShares = served
        pausedShares = paused
        blockedShares = blocked
    }

    // The cover's play/pause action: silence every share, or serve them all
    // again. Returns how many rows changed, so the caller can tell "nothing was
    // running" from a real pause.
    function pauseAllShares() {
        var changed = 0
        for (var i = 0; i < bundlesModel.count; ++i)
            if (pauseShare(i)) ++changed
        return changed
    }

    function resumeAllShares() {
        var changed = 0
        for (var i = 0; i < bundlesModel.count; ++i)
            if (resumeShare(i)) ++changed
        return changed
    }

    /*
     * The serving policy (design §12.8): when this device must not serve at all
     * — its only connection is mobile data and Wi-Fi only is on, or the battery
     * is low and it is not charging — every share that was being served stops,
     * and serves again by itself when the reason goes away.
     *
     * This is not the user's Pause switch and is not remembered as one: a row
     * carries "blocked" while it lasts, and saveShares() stores `paused` only for
     * "paused", so a restart comes back serving. The link is kept rather than
     * dropped, because it is still good — stopping a share does not move the LAN
     * listener, so the link minted when it is served again is the same link.
     */
    property bool servingBlocked: kelpPolicy.block.length > 0
    property var policyStopped: []

    // The reason as wording, for the rows. A token from C++ and a translated
    // sentence here: the one place the two meet.
    property string policyBlockText: servingBlocked
        ? (kelpPolicy.block === "metered"
           ? qsTr("not serving — on mobile data")
           : qsTr("not serving — battery low"))
        : ""

    onServingBlockedChanged: kelp.applyServingPolicy()

    function applyServingPolicy() {
        if (servingBlocked)
            suspendSharesForPolicy()
        else
            resumeSharesFromPolicy()
    }

    function suspendSharesForPolicy() {
        // Accumulated, not replaced: a share made (or resumed) while the policy
        // already says stop joins the ones already waiting, and all of them come
        // back together.
        var stopped = policyStopped.slice(0)
        for (var i = 0; i < bundlesModel.count; ++i) {
            var row = bundlesModel.get(i)
            if (row.shareState !== "seeding")
                continue
            kelpBundles.pause(row.bundleId)
            bundlesModel.setProperty(i, "shareState", "blocked")
            bundlesModel.setProperty(i, "lan", "")
            if (stopped.indexOf(row.bundleId) < 0)
                stopped.push(row.bundleId)
        }
        policyStopped = stopped
        if (stopped.length > 0) {
            kelpLog.info("KelpPolicy: " + stopped.length + " share(s) stopped — "
                         + kelpPolicy.block)
            saveShares()
        }
    }

    function resumeSharesFromPolicy() {
        var stopped = policyStopped
        policyStopped = []
        var served = 0
        for (var s = 0; s < stopped.length; ++s) {
            var index = indexOfBundle(stopped[s])
            // Gone, or paused by the user while it was stopped: leave it alone.
            if (index < 0 || bundlesModel.get(index).shareState !== "blocked")
                continue
            var row = bundlesModel.get(index)
            var published = kelpBundles.republish(row.bundleId, row.secret, row.name,
                                                  row.objects, row.files, row.bytes,
                                                  row.page === true,
                                                  row.passBlob ? row.passBlob : "")
            if (!published.ok) {
                kelpLog.warn("KelpPolicy: cannot serve \"" + row.name + "\" again: "
                             + published.error)
                continue
            }
            bundlesModel.setProperty(index, "shareState",
                                     (published.lan && published.lan.length > 0) ? "seeding"
                                                                                 : "offline")
            bundlesModel.setProperty(index, "lan", published.lan ? published.lan : "")
            bundlesModel.setProperty(index, "link", published.link)
            kelpLog.info("KelpPolicy: serving \"" + row.name + "\" again")
            ++served
        }
        if (served > 0)
            saveShares()
    }

    function indexOfBundle(bundleId) {
        for (var i = 0; i < bundlesModel.count; ++i)
            if (bundlesModel.get(i).bundleId === bundleId)
                return i
        return -1
    }

    // Brings back the saved shares and serves them again. A share is only useful
    // if its link points at somewhere reachable, and the LAN port is ephemeral,
    // so each row is republished and shows the freshly minted link. A row whose
    // stored key or id is unusable is dropped rather than restored broken. A row
    // the user had paused comes back paused: it keeps its place in the list but
    // is not served, and has no link until it is resumed.
    function restoreShares() {
        var rows = kelpShares.load()
        kelpLog.info("restoreShares: " + rows.length + " saved row(s)")
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i]
            if (row.paused === true) {
                kelpLog.info("restoreShares: leaving \"" + row.name + "\" paused")
                bundlesModel.append({
                    "name": row.name,
                    "kind": row.kind,
                    "files": row.files,
                    "size": row.size,
                    "bytes": row.bytes,
                    "path": row.path,
                    "swarm": row.swarm === true,
                    "page": row.page === true,
                    "shareState": "paused",
                    "lan": "",
                    "link": "",
                    "bundleId": row.bundleId,
                    "secret": row.secret,
                    "passBlob": row.passBlob ? row.passBlob : "",
                    "objects": row.objects
                })
                continue
            }
            var published = kelpBundles.republish(row.bundleId, row.secret, row.name,
                                                  row.objects, row.files, row.bytes,
                                                  row.page === true,
                                                  row.passBlob ? row.passBlob : "")
            if (!published.ok) {
                kelpLog.warn("restoreShares: dropping \"" + row.name + "\": " + published.error)
                continue
            }
            bundlesModel.append({
                "name": row.name,
                "kind": row.kind,
                "files": row.files,
                "size": row.size,
                "bytes": row.bytes,
                "path": row.path,
                "swarm": row.swarm === true,
                "page": row.page === true,
                "shareState": (published.lan && published.lan.length > 0) ? "seeding" : "offline",
                "lan": published.lan ? published.lan : "",
                "link": published.link,
                "bundleId": row.bundleId,
                "secret": row.secret,
                "passBlob": row.passBlob ? row.passBlob : "",
                "objects": row.objects
            })
        }
        updateShareCounters()
    }

    // Starts empty on purpose: add real files or folders from the Add page and
    // they show up here. Saved shares are put back by restoreShares() at startup,
    // so the list survives a restart.
    ListModel {
        id: bundleStore
    }

    // Exposed as a property, not just an id: the pages bind their views to
    // `store.bundlesModel`, and the future C++ store keeps the same name.
    // (Binding to an id that is not a property silently yields undefined.)
    property var bundlesModel: bundleStore

    // Bumped whenever a row changes in place (pause, resume, the address
    // changing). ListModel.get() hands back a snapshot, and a page holding one
    // will not notice a setProperty() on its own — a binding that also reads this
    // counter is re-evaluated when it moves. See BundlePage.entry.
    property int sharesRevision: 0

    // Open previews (design §5.1): a bundle's page is served on loopback while the
    // browser keeps asking for it, so a preview needs the same keep-alive a share
    // does. Kept in step with the publisher, which also drops a session that has
    // gone idle on its own.
    property int previewSessions: 0

    Connections {
        target: kelpBundles

        onPreviewSessionsChanged: kelp.previewSessions = kelpBundles.previewSessions()
    }

    // Background serving, made deliberate. Sailfish keeps a backgrounded app
    // alive, but the platform is free to end a process it thinks is idle, so
    // while something is really being served — and only then — Kelp tells the
    // system it has work to finish. Two things count, and they are different
    // users' situations: a *share* is served only because the user asked for
    // background sharing, but a *preview* is not — the party in the foreground is
    // the browser, not Kelp, and whoever opened it turned no setting on (design
    // §5.4/§5.5). A share the serving policy has stopped does not hold it either:
    // there is nothing to finish, and asking not to be ended in order to serve
    // nothing is exactly what §12.8 refuses to do.
    KeepAlive {
        id: keepAlive

        enabled: kelp.previewSessions > 0
                 || (kelpSettings.shareInBackground && kelp.servedShares > 0)
        onEnabledChanged: kelpLog.info("KeepAlive: background serving "
                                       + (enabled ? "on" : "off")
                                       + " (setting " + (kelpSettings.shareInBackground ? "on" : "off")
                                       + ", " + kelp.servedShares + " served, "
                                       + kelp.previewSessions + " previewing)")
    }

    // What the platform thinks Kelp is doing. Logged because the point of
    // background serving is what happens when the window is not in front, and
    // the log is the only place that is visible from a test on this side.
    function applicationStateName(state) {
        if (state === Qt.ApplicationActive) return "active"
        if (state === Qt.ApplicationInactive) return "inactive"
        if (state === Qt.ApplicationSuspended) return "suspended"
        if (state === Qt.ApplicationHidden) return "hidden"
        return String(state)
    }

    Connections {
        target: Qt.application

        onStateChanged: kelpLog.info("Kelp: application state = "
                                     + kelp.applicationStateName(Qt.application.state))
    }

    Component.onCompleted: {
        kelpLog.info("Kelp " + appVersion + " (build " + buildId
                     + ") UI ready; log file: " + kelpLog.filePath())
        kelp.applyRelays()                // hand the configured relay to the publisher
        kelp.restoreShares()              // serve the shares saved by earlier runs
        // A launch into a blocked state (on mobile data, or a low battery) has
        // no change to react to, so the policy is applied once explicitly —
        // restoreShares() served everything, and this stops it again.
        kelp.applyServingPolicy()
        if (initialLink.length > 0) {     // a link was given on the command line / env
            kelpLog.info("Kelp: opening the link given at startup")
            openLinkTimer.start()
        }
    }

    // Pushed after a beat so it does not race the initial page transition.
    Timer {
        id: openLinkTimer

        interval: 400
        repeat: false
        onTriggered: kelp.openLink(initialLink)
    }

    initialPage: Component { BundlesPage { store: kelp } }
    cover: Component { CoverPage { store: kelp } }
    allowedOrientations: defaultAllowedOrientations
}
