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
     * thing and the link is the only copy of the key. The rows are not
     * persisted yet: restarting forgets them, though the sealed objects stay on
     * disk.
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
    }

    // Starts empty on purpose: add real files or folders from the Add page and
    // they show up here. The rows are not persisted yet, so restarting forgets
    // the list — the sealed objects stay on disk, but nothing serves them again
    // until they are shared afresh.
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
