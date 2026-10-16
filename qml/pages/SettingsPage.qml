import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: settingsPage

    // Injected by BundlesPage.
    property var store

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

            SectionHeader { text: qsTr("Diagnostics") }

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
