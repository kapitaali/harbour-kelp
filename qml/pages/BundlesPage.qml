import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"

Page {
    id: bundlesPage

    // Injected by the ApplicationWindow (see qml/harbour-kelp.qml).
    property var store

    // The whole bundle list depends on `store.bundlesModel` actually being a
    // property; if it resolves to undefined the list is silently empty.
    Component.onCompleted: kelpLog.info("BundlesPage: store=" + (store ? "present" : "missing")
                                        + " bundlesModel="
                                        + ((store && store.bundlesModel) ? "present" : "undefined"))

    SilicaListView {
        id: listView

        anchors.fill: parent
        model: bundlesPage.store ? bundlesPage.store.bundlesModel : null
        header: PageHeader { title: qsTr("Kelp") }

        onCountChanged: kelpLog.info("BundlesPage: list count = " + count)

        PullDownMenu {
            MenuItem {
                text: qsTr("Share…")
                onClicked: {
                    kelpLog.info("BundlesPage: opening Share page")
                    pageStack.push(Qt.resolvedUrl("AddPage.qml"),
                                   { store: bundlesPage.store })
                }
            }
            MenuItem {
                text: qsTr("Open a link…")
                onClicked: {
                    kelpLog.info("BundlesPage: opening Open-link page")
                    pageStack.push(Qt.resolvedUrl("OpenLinkPage.qml"))
                }
            }
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"),
                                          { store: bundlesPage.store })
            }
            MenuItem {
                text: qsTr("About")
                onClicked: pageStack.push(Qt.resolvedUrl("AboutPage.qml"))
            }
        }

        delegate: ListItem {
            id: delegate

            width: listView.width
            contentHeight: Theme.itemSizeMedium

            Column {
                anchors {
                    left: parent.left
                    leftMargin: Theme.horizontalPageMargin
                    right: rightColumn.left
                    rightMargin: Theme.paddingMedium
                    verticalCenter: parent.verticalCenter
                }
                spacing: Theme.paddingSmall / 2

                Label {
                    width: parent.width
                    text: name
                    truncationMode: TruncationMode.Elide
                    color: delegate.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Elide
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: (page === true ? qsTr("Web page") + " · " : "")
                          + (kind === "folder" ? qsTr("%1 files").arg(files)
                                               : qsTr("%1 file").arg(files))
                          + " · " + size
                }
            }

            // State and where the objects can be fetched from, stacked. Encryption
            // is not shown: every bundle is encrypted, so it carries no
            // information here. The address is the useful part — it is the
            // difference between a link that works and one that does not.
            Column {
                id: rightColumn
                anchors {
                    right: parent.right
                    rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                spacing: Theme.paddingSmall / 2

                Label {
                    anchors.right: parent.right
                    text: shareState === "seeding" ? qsTr("Seeding")
                          : (shareState === "paused" ? qsTr("Paused")
                          : (shareState === "blocked" ? qsTr("Not serving") : qsTr("Offline")))
                    color: shareState === "seeding" ? Theme.highlightColor : Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }

                Label {
                    anchors.right: parent.right
                    // A stopped-by-policy row says why, in the place where an
                    // address would otherwise be: that is the difference between
                    // "something is wrong" and "this will come back by itself".
                    text: lan.length > 0 ? lan
                          : (shareState === "blocked" && bundlesPage.store
                             && bundlesPage.store.policyBlockText.length > 0
                             ? bundlesPage.store.policyBlockText : qsTr("not serving"))
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                }
            }

            menu: ContextMenu {
                MenuItem {
                    text: qsTr("Open")
                    onClicked: pageStack.push(Qt.resolvedUrl("BundlePage.qml"),
                                              { store: bundlesPage.store, index: index })
                }
                MenuItem {
                    text: qsTr("Copy link")
                    onClicked: {
                        Clipboard.text = link
                        toast.show(qsTr("Link copied"))
                    }
                }
                MenuItem {
                    text: shareState === "paused" ? qsTr("Resume sharing") : qsTr("Pause sharing")
                    onClicked: {
                        if (shareState === "paused")
                            bundlesPage.store.resumeShare(index)
                        else
                            bundlesPage.store.pauseShare(index)
                    }
                }
                MenuItem {
                    text: qsTr("Stop sharing")
                    onClicked: {
                        var doomed = index
                        var bundleId = bundlesPage.store.bundlesModel.get(doomed).bundleId
                        kelpLog.info("Stop sharing: remorse started for index " + doomed)
                        remorse.execute(qsTr("Stopping sharing"), function() {
                            kelpLog.info("Stop sharing: removing index " + doomed
                                         + " (count " + bundlesPage.store.bundlesModel.count + ")")
                            // Stop serving before the row goes away, or the
                            // bundle would stay fetchable with nothing in the
                            // list to stop it.
                            kelpBundles.unshare(bundleId)
                            bundlesPage.store.bundlesModel.remove(doomed)
                            bundlesPage.store.saveShares()
                        })
                    }
                }
            }

            onClicked: pageStack.push(Qt.resolvedUrl("BundlePage.qml"),
                                      { store: bundlesPage.store, index: index })
        }

        ViewPlaceholder {
            enabled: listView.count === 0
            text: qsTr("No shared bundles")
            hintText: qsTr("Pull down to share a file, a folder, or a webpage")
        }

        VerticalScrollDecorator {}
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
