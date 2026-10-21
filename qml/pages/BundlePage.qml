import QtQuick 2.0
import Sailfish.Silica 1.0
import harbour.kelp 1.0
import "../components"

Page {
    id: bundlePage

    // Injected by BundlesPage: the store and the row index in its model.
    property var store
    property int index

    // ListModel.get() returns a snapshot object; read it once and guard, since
    // the row can disappear if sharing is stopped elsewhere. The dependence on
    // `store.sharesRevision` is what makes this re-read after a pause or resume:
    // a setProperty() on the model does not refresh a plain get() by itself.
    property var entry: (store && index >= 0 && store.sharesRevision >= 0)
                        ? store.bundlesModel.get(index) : null

    // A share the user silenced. It is still in the list and still holds its
    // objects, but nothing is served and it has no live link.
    property bool paused: entry ? entry.shareState === "paused" : false

    // A share the serving policy stopped (design §12.8): the device is on mobile
    // data with Wi-Fi only on, or the battery is low and not charging. Unlike a
    // pause this is nobody's choice and it undoes itself — and unlike a pause the
    // link is kept, because the listener did not move and the link still works.
    property bool policyBlocked: entry ? entry.shareState === "blocked" : false

    // The link, decoded rather than string-split, so the page shows what a
    // recipient's client would see in it.
    property var linkParts: entry ? kelpBundles.linkInfo(entry.link) : null

    /*
     * Where the objects can be fetched from, which is not part of the hash: the
     * address is a hint carried in the link's query. Today that is only the
     * sender's LAN address, when it is listening; the app does not configure a
     * relay.
     */
    function whereText() {
        if (!linkParts || !linkParts.ok)
            return qsTr("unknown")
        return linkParts.lan.length > 0 ? qsTr("LAN %1").arg(linkParts.lan)
                                        : qsTr("(none yet)")
    }

    // The one address that matters on this page: what a recipient would dial.
    function servedAddress() {
        if (!linkParts || !linkParts.ok)
            return qsTr("unknown")
        return linkParts.lan.length > 0 ? linkParts.lan : qsTr("not being served")
    }

    // The per-share swarm choice is recorded, but nothing acts on it yet, so the
    // page says so rather than implying peers are involved.
    property string swarmNote: (entry && entry.swarm)
        ? qsTr("\n\nPeer swarm was requested for this share. It is not implemented yet, so no one is trading these objects with anyone.")
        : ""

    // Which file in this bundle a browser would be shown (design §5.3), asked once
    // when the page opens. Which file that is cannot change under us, so it is
    // asked once; the *kind* of share is the publisher's own hint (design §5.5),
    // carried in the link as an encrypted `q=`, and `entry.page` is that hint.
    property var page: null

    // Whether this share was *declared* as a webpage (design §5.5). A page being in
    // the bundle is not enough: the kind the publisher chose decides what the app
    // offers — here as on the receive side, where an ordinary link only ever saves.
    // A folder share is files to download, and a page among them is the recipient's
    // own business, opened from their files (the user's decision, 2026-10-18, §3.26).
    property bool isWebpage: !!bundlePage.entry && bundlePage.entry.page === true
                             && bundlePage.page !== null && bundlePage.page.hasPage

    // The browser section is webpage-only: a page can be previewed locally. A
    // folder has no page to run, and there is no relay address to copy.

    function lookForPage() {
        page = null
        if (!bundlePage.entry || !bundlePage.entry.secret || bundlePage.entry.secret.length === 0)
            return
        var info = kelpBundles.pageInfo(bundlePage.entry.bundleId, bundlePage.entry.secret)
        page = info
        if (!info.ok) {
            // Not "no page": the bundle's own manifests could not be read, so there
            // is nothing to say about it yet.
            kelpLog.warn("BundlePage: cannot look for a page in \""
                         + bundlePage.entry.name + "\": " + info.error)
        } else if (info.hasPage) {
            kelpLog.info("BundlePage: \"" + bundlePage.entry.name + "\" offers a page: "
                         + info.entry)
        }
    }

    Component.onCompleted: {
        kelpLog.info("BundlePage: index=" + index + " entry="
                     + (entry ? "\"" + entry.name + "\"" : "null"))
        lookForPage()
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: bundlePage.entry ? bundlePage.entry.name : qsTr("Bundle") }

            SectionHeader { text: qsTr("Bundle") }

            DetailItem {
                width: parent.width
                label: qsTr("Files")
                value: bundlePage.entry ? String(bundlePage.entry.files) : "-"
            }
            DetailItem {
                width: parent.width
                label: qsTr("Size")
                value: bundlePage.entry ? bundlePage.entry.size : "-"
            }
            DetailItem {
                width: parent.width
                label: qsTr("Location")
                value: bundlePage.entry ? bundlePage.entry.path : "-"
            }
            DetailItem {
                width: parent.width
                label: qsTr("Objects")
                value: bundlePage.entry ? String(bundlePage.entry.objects) : "-"
            }
            DetailItem {
                width: parent.width
                label: qsTr("Status")
                value: bundlePage.paused
                       ? qsTr("Paused")
                       : (bundlePage.policyBlocked
                          ? qsTr("Stopped — %1").arg(bundlePage.store
                                                    ? bundlePage.store.policyBlockText
                                                    : qsTr("not serving"))
                          : ((bundlePage.entry && bundlePage.entry.shareState === "seeding")
                             ? qsTr("Seeding") : qsTr("Offline")))
            }
            DetailItem {
                width: parent.width
                label: qsTr("Served on")
                value: bundlePage.servedAddress()
            }
            DetailItem {
                width: parent.width
                label: qsTr("Encryption")
                value: qsTr("Always on · key in the link")
            }

            SectionHeader { text: qsTr("Share link") }

            Label {
                visible: bundlePage.paused
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                text: qsTr("This share is paused, so there is no link to send. Resume it to serve the objects again — resuming mints a fresh link, because the address is only good while the bundle is being served.")
            }

            // A policy stop keeps the link, so this page says so rather than
            // hiding it: the recipient holding it is not left with a dead link.
            Label {
                visible: bundlePage.policyBlocked
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                text: kelpPolicy.block === "metered"
                      ? qsTr("Serving is stopped because this device's only connection is mobile data and “Wi-Fi only” is on. The link below is unchanged and starts answering again by itself when Wi-Fi (or a wired network) is back.")
                      : qsTr("Serving is stopped because the battery is low. The link below is unchanged and starts answering again by itself when this device is charging.")
            }

            TextArea {
                id: linkArea
                visible: !bundlePage.paused
                width: parent.width
                readOnly: true
                text: bundlePage.entry ? bundlePage.entry.link : ""
                label: qsTr("Send this to the recipient")
            }

            // A kelp:// link is three parts: the content address, where to fetch
            // the objects from, and the key. Spelling them out makes it obvious
            // which one is missing — the middle one is the LAN address, because
            // the app does not configure a relay.
            Label {
                visible: !bundlePage.paused
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: (bundlePage.entry && bundlePage.linkParts && bundlePage.linkParts.ok)
                      ? qsTr("Identity\n%1\n\nWhere\n%2\n\nKey\n%3")
                        .arg(bundlePage.linkParts.bundleId)
                        .arg(bundlePage.whereText())
                        .arg(qsTr("after '#'. A fragment is never sent to a server, so the relay and any peers see only ciphertext — but anyone holding the link can decrypt the bundle."))
                      : qsTr("This link cannot be read.")
            }

            Label {
                visible: !bundlePage.paused
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("The identity is a hash of the sealed objects, so it names the content and says nothing about where it is kept.")
            }

            // Wrapped in an Item so the buttons keep the page margin inside the
            // Column (positioners ignore child anchors).
            Item {
                width: parent.width
                height: buttonColumn.height

                Column {
                    id: buttonColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Button {
                        width: parent.width
                        text: qsTr("Copy link")
                        enabled: !bundlePage.paused
                        onClicked: {
                            if (bundlePage.entry) {
                                Clipboard.text = bundlePage.entry.link
                                toast.show(qsTr("Link copied"))
                            }
                        }
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Show QR code")
                        enabled: !bundlePage.paused
                        onClicked: qrDialog.open()
                    }
                    Button {
                        width: parent.width
                        text: bundlePage.paused ? qsTr("Resume sharing")
                                                : qsTr("Pause sharing")
                        onClicked: {
                            if (bundlePage.paused) {
                                if (bundlePage.store.resumeShare(bundlePage.index))
                                    toast.show(qsTr("Sharing resumed"))
                            } else {
                                if (bundlePage.store.pauseShare(bundlePage.index))
                                    toast.show(qsTr("Sharing paused"))
                            }
                        }
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Stop sharing")
                        onClicked: {
                            var doomed = bundlePage.index
                            var bundleId = bundlePage.entry ? bundlePage.entry.bundleId : ""
                            kelpLog.info("Stop sharing: remorse started for index " + doomed)
                            remorse.execute(qsTr("Stopping sharing"), function() {
                                kelpLog.info("Stop sharing: removing index " + doomed)
                                // Stop serving before the row goes away, or the
                                // bundle would stay fetchable with nothing in
                                // the list to stop it.
                                kelpBundles.unshare(bundleId)
                                bundlePage.store.bundlesModel.remove(doomed)
                                bundlePage.store.saveShares()
                                pageStack.pop()
                            })
                        }
                    }
                }
            }

            // The local preview (design §5.2): only a webpage has a page to run.
            // A folder is files, not a browser view, and there is no relay
            // address to offer.
            SectionHeader {
                visible: bundlePage.isWebpage
                text: qsTr("In a browser")
            }

            Label {
                visible: bundlePage.isWebpage
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: {
                    if (!bundlePage.isWebpage)
                        return ""
                    var text = qsTr("This bundle has a page in it: %1. It opens in this device's browser, which is the only place a page can run — the app decrypts the bundle and serves it to the browser from 127.0.0.1 for as long as the browser keeps asking.")
                                   .arg(bundlePage.page.entry)
                    text += qsTr(" The link says this share is a webpage, so a recipient that understands it is invited to open the page rather than keep a copy.")
                    if (bundlePage.page.candidates
                            && bundlePage.page.candidates.length > 1) {
                        text += qsTr(" It holds %1 pages; the one above is the one shown.")
                                    .arg(bundlePage.page.candidates.length)
                    }
                    return text
                }
            }

            // Wrapped in an Item so the buttons keep the page margin inside the
            // Column (positioners ignore child anchors).
            Item {
                visible: bundlePage.isWebpage
                width: parent.width
                height: webButtons.height

                Column {
                    id: webButtons
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Button {
                        id: previewButton
                        visible: bundlePage.isWebpage
                        width: parent.width
                        text: qsTr("Open in browser")
                        onClicked: {
                            if (!bundlePage.entry)
                                return
                            var opened = kelpBundles.previewPage(bundlePage.entry.bundleId,
                                                                 bundlePage.entry.secret,
                                                                 bundlePage.entry.name,
                                                                 bundlePage.page ? bundlePage.page.entry : "")
                            if (!opened.ok) {
                                kelpLog.warn("Preview: \"" + bundlePage.entry.name + "\": "
                                             + opened.error)
                                toast.show(opened.error.length > 0
                                           ? opened.error
                                           : qsTr("Could not open the page"))
                                return
                            }
                            // Logged without the URL: the address carries the token that
                            // is the capability (the app writes it to the log only in a
                            // dev build that asks for links in the log).
                            kelpLog.info("Preview: serving \"" + opened.entry
                                         + "\" and handing it to the browser")
                            Qt.openUrlExternally(opened.url)
                            toast.show(qsTr("Opening in the browser"))
                        }
                    }

                }
            }

            SectionHeader { text: qsTr("Sources") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: bundlePage.paused
                      ? qsTr("This share is paused. It stays in the list and its objects stay in the store, but nothing is served, so any link handed out earlier will not work until you resume it.")
                      : (bundlePage.policyBlocked
                         ? qsTr("Serving is stopped for now, so nobody can fetch this bundle. It is still sealed in the object store on this device, and the link above starts answering again by itself.")
                         : (bundlePage.entry && bundlePage.linkParts && bundlePage.linkParts.ok
                            && bundlePage.linkParts.lan
                            && bundlePage.linkParts.lan.length > 0
                            ? qsTr("Anyone on this Wi-Fi who has the link can fetch these objects while the app is sharing them. A recipient on another network cannot: this app does not configure a relay.")
                            : qsTr("This bundle is not being served, so nobody can fetch it. It is still sealed in the object store on this device.")))
                      + bundlePage.swarmNote
            }
        }
    }

    // The link as a QR code: the shortest way to get a share from this device to
    // another. It carries the same key the text link does, so it is shown only on
    // demand, here.
    Dialog {
        id: qrDialog
        objectName: "kelpQrDialog"

        Column {
            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader {
                title: qsTr("Share link")
                acceptText: qsTr("Close")
            }

            // Wrapped in an Item: a Column is a positioner and ignores the
            // anchors of its own children.
            Item {
                width: parent.width
                height: qrBox.height

                Rectangle {
                    id: qrBox
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(parent.width - 2 * Theme.horizontalPageMargin, 3 * Theme.itemSizeLarge)
                    height: width
                    // White behind the code, never themed: a scanner wants contrast.
                    color: "white"

                    KelpQrCode {
                        id: qrImage
                        objectName: "kelpQrCode"
                        anchors.fill: parent
                        anchors.margins: Theme.paddingSmall
                        text: bundlePage.entry ? bundlePage.entry.link : ""
                    }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Theme.paddingLarge
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: "black"
                        visible: !qrImage.valid
                        text: qsTr("This link is too long to show as a QR code.")
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Scan this with Kelp on the other device. The code holds the same key as the link.")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WrapAnywhere
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: bundlePage.entry ? bundlePage.entry.link : ""
            }
        }
    }

    RemorsePopup {
        id: remorse

        // Diagnosable from the log: "canceled" means the tap-to-undo was seen,
        // "triggered" means the action actually ran.
        onTriggered: kelpLog.info("remorse: triggered — action will run")
        onCanceled: kelpLog.info("remorse: canceled — undo was honoured")
    }
    Toast { id: toast }
}
