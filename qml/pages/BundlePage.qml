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
    // the row can disappear if sharing is stopped elsewhere.
    property var entry: (store && index >= 0) ? store.bundlesModel.get(index) : null

    // The link, decoded rather than string-split, so the page shows what a
    // recipient's client would see in it.
    property var linkParts: entry ? kelpBundles.linkInfo(entry.link) : null

    /*
     * Where the objects can be fetched from, which is not part of the hash: the
     * address is a hint carried in the link's query. Today that is the sender's
     * LAN address, when it is listening; relays come later.
     */
    function whereText() {
        if (!linkParts || !linkParts.ok)
            return qsTr("unknown")
        var found = []
        if (linkParts.lan.length > 0)
            found.push(qsTr("LAN %1").arg(linkParts.lan))
        for (var i = 0; i < linkParts.relays.length; ++i)
            found.push(qsTr("relay %1").arg(linkParts.relays[i]))
        return found.length > 0 ? found.join(qsTr(", "))
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

    Component.onCompleted: kelpLog.info("BundlePage: index=" + index + " entry="
                                        + (entry ? "\"" + entry.name + "\"" : "null"))

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
                value: (bundlePage.entry && bundlePage.entry.state === "seeding")
                       ? qsTr("Seeding") : qsTr("Offline")
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

            TextArea {
                id: linkArea
                width: parent.width
                readOnly: true
                text: bundlePage.entry ? bundlePage.entry.link : ""
                label: qsTr("Send this to the recipient")
            }

            // A kelp:// link is three parts: the content address, where to fetch
            // the objects from, and the key. Spelling them out makes it obvious
            // which one is missing — the middle one, until the relay lands and a
            // link works away from the local network too.
            Label {
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
                        onClicked: qrDialog.open()
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

            SectionHeader { text: qsTr("Sources") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: (bundlePage.entry && bundlePage.linkParts
                      && bundlePage.linkParts.lan.length > 0
                      ? qsTr("Anyone on this Wi-Fi who has the link can fetch these objects while the app is sharing them. Nothing is reachable from outside the local network yet — that is what the relay will add.")
                      : qsTr("This bundle is not being served, so nobody can fetch it. It is still sealed in the object store on this device."))
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
