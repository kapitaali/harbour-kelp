import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: aboutPage

    // Walking the object store and hashing entries is not free, so read it once
    // when the page is built rather than on every binding evaluation.
    property var storeInfo: kelpBundles.storeInfo()

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("About Kelp") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.primaryColor
                font.pixelSize: Theme.fontSizeMedium
                text: "Kelp " + appVersion
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Build %1. Content-addressed peer-to-peer file sharing: share a folder or file as a kelp:// link, always encrypted.").arg(buildId)
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Files are chunked, encrypted with AES-256-GCM and stored as SHA-256-addressed objects, and the kelp:// link carries the key. Kelp serves a shared bundle to anyone holding the link on the same local network, and a recipient checks every object against its address before keeping it. A relay is used as a fallback when a recipient cannot reach this device directly. Peer swarm is not implemented yet.")
            }

            SectionHeader { text: qsTr("Diagnostics") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Log file: %1").arg(kelpLog.filePath())
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: (aboutPage.storeInfo && aboutPage.storeInfo.ok)
                      ? qsTr("Object store: %1 object(s), %2\n%3")
                        .arg(aboutPage.storeInfo.objects)
                        .arg(kelpFiles.formatSize(aboutPage.storeInfo.bytes))
                        .arg(aboutPage.storeInfo.path)
                      : qsTr("Object store: unavailable")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: (aboutPage.storeInfo && aboutPage.storeInfo.serving
                       && aboutPage.storeInfo.serving.length > 0)
                      ? qsTr("Serving shared bundles on %1 (local network only).")
                        .arg(aboutPage.storeInfo.serving)
                      : qsTr("Not serving anything: no bundle is shared, or there is no local address.")
            }
        }
    }
}
