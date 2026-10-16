import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: settingsPage

    // Injected by BundlesPage.
    property var store

    // Settings → Storage. Filled in when the page becomes active — walking the
    // store and the share trees is not free, and `store` is only injected as the
    // page is pushed, so reading it at build time could see no shares at all.
    property var usage

    Component.onCompleted: refreshUsage()
    onStatusChanged: {
        if (status === PageStatus.Active)
            refreshUsage()
    }

    // What the Shares page is showing, in the shape storeUsage() and pruneStore()
    // want. Passed in rather than read back from the shares file, so "in use" is
    // exactly what this device is really serving.
    function shareRows() {
        var rows = []
        if (!settingsPage.store)
            return rows
        var model = settingsPage.store.bundlesModel
        for (var i = 0; i < model.count; ++i) {
            var row = model.get(i)
            rows.push({ "bundleId": row.bundleId, "secret": row.secret })
        }
        return rows
    }

    function refreshUsage() {
        settingsPage.usage = kelpBundles.storeUsage(settingsPage.shareRows())
    }

    function usageText() {
        if (!settingsPage.usage)
            return qsTr("Looking at the object store…")
        if (!settingsPage.usage.ok) {
            return qsTr("Could not read the object store, so nothing can be removed: %1")
                .arg(settingsPage.usage.error)
        }
        return qsTr("The store holds %1 object(s), %2.\n%3 of them, %4, belong to the shares on this device. The other %5, %6, are cached leftovers — what a fetch left behind, or what a share no longer needs. Removing them frees the space; anything fetched from a link again is downloaded once more.")
            .arg(settingsPage.usage.objects)
            .arg(kelpFiles.formatSize(settingsPage.usage.bytes))
            .arg(settingsPage.usage.inUseObjects)
            .arg(kelpFiles.formatSize(settingsPage.usage.inUseBytes))
            .arg(settingsPage.usage.unusedObjects)
            .arg(kelpFiles.formatSize(settingsPage.usage.unusedBytes))
    }

    // Asks before deleting, and only ever offers what storeUsage() called unused.
    Dialog {
        id: confirmPrune

        acceptDestinationAction: PageStackAction.Pop

        Column {
            width: parent.width
            spacing: Theme.paddingMedium

            DialogHeader { title: qsTr("Remove unused objects") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                text: qsTr("Delete the %1 object(s), %2, that no share on this device needs? No share is affected; anything you fetch again is downloaded once more.")
                    .arg(settingsPage.usage ? settingsPage.usage.unusedObjects : 0)
                    .arg(settingsPage.usage
                         ? kelpFiles.formatSize(settingsPage.usage.unusedBytes) : "")
            }
        }

        onAccepted: {
            if (kelpClient.busy) {
                kelpLog.warn("SettingsPage: clean-up skipped, a download is running")
                return
            }
            var result = kelpBundles.pruneStore(settingsPage.shareRows())
            if (result.ok) {
                kelpLog.info("SettingsPage: removed " + result.removed
                             + " unused object(s), " + result.freedBytes + " bytes")
            } else {
                kelpLog.warn("SettingsPage: clean-up refused: " + result.error)
            }
            settingsPage.refreshUsage()
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Settings") }

            SectionHeader { text: qsTr("Sharing") }

            TextSwitch {
                text: qsTr("Include my local address in the link")
                description: qsTr("Seal this device's LAN address and the share name into every link, encrypted with the link's own key, and serve the bundle there for a recipient on the same network. Whoever holds the whole link can still open it; anyone who sees the link without its #k= part cannot. Turn it off to keep the address out of the link entirely — the bundle is then not served on the LAN and only a relay can carry it.")
                checked: kelpSettings.includeLanAddress
                onCheckedChanged: {
                    if (checked !== kelpSettings.includeLanAddress)
                        kelpSettings.includeLanAddress = checked
                }
            }

            TextSwitch {
                text: qsTr("Share in background")
                description: qsTr("Keep seeding after the app is closed")
                checked: settingsPage.store ? settingsPage.store.seedingEnabled : false
                onCheckedChanged: {
                    if (settingsPage.store && checked !== settingsPage.store.seedingEnabled)
                        settingsPage.store.seedingEnabled = checked
                }
            }

            TextSwitch {
                text: qsTr("Wi-Fi only")
                description: qsTr("Never seed over mobile data")
                checked: settingsPage.store ? settingsPage.store.wifiOnly : true
                onCheckedChanged: {
                    if (settingsPage.store && checked !== settingsPage.store.wifiOnly)
                        settingsPage.store.wifiOnly = checked
                }
            }

            SectionHeader { text: qsTr("Swarm") }

            TextSwitch {
                text: qsTr("Allow peer swarm")
                description: qsTr("Permit a bundle to let other downloaders serve pieces to each other. Still off per share by default, and those peers see your IP address.")
                checked: settingsPage.store ? settingsPage.store.swarmEnabled : false
                onCheckedChanged: {
                    if (settingsPage.store && checked !== settingsPage.store.swarmEnabled)
                        settingsPage.store.swarmEnabled = checked
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("This is a permission, not a per-bundle setting: enable it here, then choose per share. Bundles stay encrypted either way — peers only ever exchange ciphertext they cannot read.")
            }

            SectionHeader { text: qsTr("Relay") }

            TextField {
                width: parent.width
                label: qsTr("Relay address")
                placeholderText: qsTr("host:port — leave empty for LAN only")
                text: settingsPage.store ? settingsPage.store.relayUrl : ""
                onTextChanged: {
                    if (settingsPage.store && text !== settingsPage.store.relayUrl)
                        settingsPage.store.relayUrl = text
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Where bundles are published so recipients on other networks can fetch them. Empty means LAN only. The relay forwards ciphertext it cannot read, and is given only a key of its own — not the content key.")
            }

            SectionHeader { text: qsTr("Storage") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: settingsPage.usageText()
            }

            Button {
                width: parent.width
                text: qsTr("Remove unused objects")
                // Not while a fetch is running: its objects are not in use yet, and
                // deleting them mid-transfer would break the download.
                enabled: settingsPage.usage && settingsPage.usage.ok
                         && settingsPage.usage.unusedObjects > 0 && !kelpClient.busy
                onClicked: confirmPrune.open()
            }

            Label {
                visible: kelpClient.busy
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("A download is running, so nothing can be removed just now.")
            }

            SectionHeader { text: qsTr("Diagnostics") }

            TextSwitch {
                text: qsTr("Include links in the log")
                description: qsTr("Write each share's kelp:// link to the log file. A link holds the key to that bundle, so anyone able to read the log can open it. Leave this off unless you need it — it is the only way to read a link off a device with no usable clipboard.")
                checked: kelpSettings.includeLinksInLog
                onCheckedChanged: {
                    if (checked !== kelpSettings.includeLinksInLog)
                        kelpSettings.includeLinksInLog = checked
                }
            }

            Button {
                width: parent.width
                text: qsTr("Clear log")
                onClicked: {
                    kelpLog.clear()
                    kelpLog.info("SettingsPage: log cleared by user")
                }
            }
        }
    }
}
