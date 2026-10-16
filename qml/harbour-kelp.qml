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
    // app exits, so it is reminted by restoreShares() instead.
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
                "bundleId": row.bundleId,
                "secret": row.secret
            })
        }
        kelpLog.info("saveShares: writing " + rows.length + " row(s)")
        kelpShares.save(rows)
    }

    // Brings back the saved shares and serves them again. A share is only useful
    // if its link points at somewhere reachable, and the LAN port is ephemeral,
    // so each row is republished and shows the freshly minted link. A row whose
    // stored key or id is unusable is dropped rather than restored broken.
    function restoreShares() {
        var rows = kelpShares.load()
        kelpLog.info("restoreShares: " + rows.length + " saved row(s)")
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i]
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
