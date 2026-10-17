import QtQuick 2.0
import Sailfish.Silica 1.0
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
    property bool seedingEnabled: true
    property bool wifiOnly: true
    property bool swarmEnabled: false

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

    function addBundle(info) {
        kelpLog.info("addBundle: name=\"" + info.name + "\" kind=" + info.kind
                     + " files=" + info.files + " size=" + info.size
                     + " path=" + info.path + " objects=" + info.objects
                     + " lan=" + info.lan + " swarm=" + (info.swarm === true))
        bundlesModel.append({
            "name": info.name,
            "kind": info.kind,
            "files": info.files,
            "size": info.size,
            "bytes": info.bytes,
            "path": info.path,
            "swarm": info.swarm === true,
            // "Seeding" only means something when there is an address to serve
            // on: that is the difference between a link that works and one that
            // does not.
            "state": (info.lan && info.lan.length > 0) ? "seeding" : "offline",
            "lan": info.lan ? info.lan : "",
            "link": info.link,
            "bundleId": info.bundleId,
            "secret": info.secret,
            "objects": info.objects
        })
        kelpLog.info("addBundle: bundlesModel.count is now " + bundlesModel.count)
        saveShares()
    }

    // Writes the row fields the app must remember across a restart. The link
    // itself is not saved: it embeds an address that goes stale the moment the
    // app exits, so it is reminted by restoreShares() instead. `paused` IS saved,
    // though — a share the user silenced should stay silent, not come back
    // serving itself at the next launch.
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
                "objects": row.objects,
                "paused": row.state === "paused",
                "bundleId": row.bundleId,
                "secret": row.secret
            })
        }
        kelpLog.info("saveShares: writing " + rows.length + " row(s)")
        kelpShares.save(rows)
        sharesRevision++     // a row may have changed in place; pages re-read it
    }

    // Silences a share without forgetting it: stop serving (LAN and relay), keep
    // the row and the objects. The address in the link is now dead, so the link
    // is dropped with it; resuming mints a fresh one. Returns false if there is
    // nothing at that row or it is already paused.
    function pauseShare(index) {
        if (!bundlesModel || index < 0 || index >= bundlesModel.count)
            return false
        var row = bundlesModel.get(index)
        if (row.state === "paused")
            return false
        kelpLog.info("pauseShare: pausing \"" + row.name + "\"")
        kelpBundles.pause(row.bundleId)
        bundlesModel.setProperty(index, "state", "paused")
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
        if (row.state !== "paused")
            return false
        var published = kelpBundles.republish(row.bundleId, row.secret, row.name,
                                              row.objects, row.files, row.bytes)
        if (!published.ok) {
            kelpLog.warn("resumeShare: cannot resume \"" + row.name + "\": " + published.error)
            return false
        }
        bundlesModel.setProperty(index, "state",
                                 (published.lan && published.lan.length > 0) ? "seeding" : "offline")
        bundlesModel.setProperty(index, "lan", published.lan ? published.lan : "")
        bundlesModel.setProperty(index, "link", published.link)
        saveShares()
        return true
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
                    "state": "paused",
                    "lan": "",
                    "link": "",
                    "bundleId": row.bundleId,
                    "secret": row.secret,
                    "objects": row.objects
                })
                continue
            }
            var published = kelpBundles.republish(row.bundleId, row.secret, row.name,
                                                  row.objects, row.files, row.bytes)
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
                "state": (published.lan && published.lan.length > 0) ? "seeding" : "offline",
                "lan": published.lan ? published.lan : "",
                "link": published.link,
                "bundleId": row.bundleId,
                "secret": row.secret,
                "objects": row.objects
            })
        }
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

    Component.onCompleted: {
        kelpLog.info("Kelp " + appVersion + " (build " + buildId
                     + ") UI ready; log file: " + kelpLog.filePath())
        kelp.applyRelays()                // hand the configured relay to the publisher
        kelp.restoreShares()              // serve the shares saved by earlier runs
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
        onTriggered: pageStack.push(Qt.resolvedUrl("pages/OpenLinkPage.qml"),
                                    { link: initialLink })
    }

    initialPage: Component { BundlesPage { store: kelp } }
    cover: Component { CoverPage { store: kelp } }
    allowedOrientations: defaultAllowedOrientations
}
