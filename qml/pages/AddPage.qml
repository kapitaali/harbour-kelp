import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import "../components"

/*
 * Add content to share.
 *
 * Both pickers walk the real filesystem. Files use the packaged FilePickerPage
 * (with a catch-all name filter, since its default filter matches nothing).
 * Folders use FolderPickPage: the packaged FolderPickerPage picks the current
 * folder through a blank header button, which is hard to find. Picking is
 * additive, so tapping "Choose files…" repeatedly builds up a multi-file
 * selection.
 *
 * Nothing is sealed until "Start sharing": the selection is only a list of
 * paths until then, and each entry becomes one bundle with its own link.
 */
Page {
    id: addPage

    // Injected by BundlesPage.
    property var store

    // Whether peers may help serve the bundles added here. Off by default and
    // gated by the global "Allow peer swarm" permission in Settings.
    property bool swarm: false

    // True while startSharing() is sealing, so the button cannot be pressed
    // twice. Sealing blocks the UI thread, so this never actually paints.
    property bool sealing: false

    // Each entry: { name, path, kind, fileCount, totalBytes }
    property var selection: []

    function addEntry(name, path, kind, fileCount, totalBytes) {
        kelpLog.info("AddPage: selecting " + kind + " \"" + name + "\" (" + path + "), "
                     + fileCount + " file(s), " + kelpFiles.formatSize(totalBytes))
        var next = selection.slice()
        next.push({ "name": name, "path": path, "kind": kind,
                    "fileCount": fileCount, "totalBytes": totalBytes })
        selection = next
        kelpLog.info("AddPage: selection now holds " + selection.length + " entr(ies)")
    }

    function removeEntry(index) {
        var next = selection.slice()
        next.splice(index, 1)
        selection = next
    }

    function selectedFileCount() {
        var count = 0
        for (var i = 0; i < selection.length; ++i)
            count += selection[i].fileCount
        return count
    }

    function selectedBytes() {
        var bytes = 0
        for (var i = 0; i < selection.length; ++i)
            bytes += selection[i].totalBytes
        return bytes
    }

    /*
     * Seals every selected entry and adds it to the bundle list. One entry
     * becomes one bundle, so a five-file selection yields five links.
     *
     * Sealing is synchronous — it reads the files, encrypts them and writes the
     * objects — so the UI is frozen for the duration; `sealing` only exists so
     * the button cannot be pressed twice. On failure it stops, keeps the page
     * open and says why, because the entries are still selected and the user
     * will want to retry.
     */
    function startSharing() {
        if (sealing || selection.length === 0)
            return

        var peerSwarm = swarm && store && store.swarmEnabled
        kelpLog.info("AddPage: Start sharing pressed — " + selection.length
                     + " entr(ies), " + selectedFileCount() + " file(s) total, swarm="
                     + peerSwarm)

        sealing = true
        var skipped = 0
        for (var i = 0; i < selection.length; ++i) {
            var entry = selection[i]
            var sealed = kelpBundles.seal([ entry.path ], entry.name)
            if (!sealed.ok) {
                kelpLog.error("AddPage: sealing \"" + entry.name + "\" failed: "
                              + sealed.error)
                toast.show(qsTr("Could not share %1: %2").arg(entry.name)
                                                          .arg(sealed.error))
                sealing = false
                return
            }
            skipped += sealed.skippedLinks
            kelpLog.info("AddPage: sealed \"" + entry.name + "\" — "
                         + sealed.objectCount + " object(s), " + sealed.fileCount
                         + " file(s), " + sealed.totalBytes + " bytes, id="
                         + sealed.bundleId)
            store.addBundle({
                "name": entry.name,
                "kind": entry.kind,
                "files": sealed.fileCount,
                "size": kelpFiles.formatSize(sealed.totalBytes),
                "path": entry.path,
                "swarm": peerSwarm,
                "lan": sealed.lan,
                "link": sealed.link,
                "bundleId": sealed.bundleId,
                "secret": sealed.secret,
                "objects": sealed.objectCount
            })
        }
        // Symlinks are skipped rather than followed, so a link-heavy folder can
        // come out smaller than it looked. Nothing else surfaces that yet.
        if (skipped > 0)
            kelpLog.warn("AddPage: skipped " + skipped
                         + " symbolic link(s) while sealing")

        sealing = false
        pageStack.pop()
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Add content") }

            SectionHeader { text: qsTr("Source") }

            Label {
                visible: addPage.selection.length === 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Nothing selected yet. Pick one or more files, or a whole folder.")
            }

            Repeater {
                model: addPage.selection

                delegate: ListItem {
                    id: selectionItem

                    width: column.width
                    contentHeight: Theme.itemSizeMedium

                    Column {
                        anchors {
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                            right: sizeLabel.left
                            rightMargin: Theme.paddingMedium
                            verticalCenter: parent.verticalCenter
                        }
                        spacing: Theme.paddingSmall / 2

                        Label {
                            width: parent.width
                            text: modelData.name
                            truncationMode: TruncationMode.Elide
                            color: selectionItem.highlighted ? Theme.highlightColor
                                                             : Theme.primaryColor
                        }

                        Label {
                            width: parent.width
                            text: modelData.path
                            truncationMode: TruncationMode.Elide
                            color: Theme.secondaryColor
                            font.pixelSize: Theme.fontSizeExtraSmall
                        }
                    }

                    Label {
                        id: sizeLabel
                        anchors {
                            right: parent.right
                            rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        text: kelpFiles.formatSize(modelData.totalBytes)
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                    }

                    menu: ContextMenu {
                        MenuItem {
                            text: qsTr("Remove")
                            onClicked: addPage.removeEntry(index)
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: pickerColumn.height

                Column {
                    id: pickerColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Button {
                        width: parent.width
                        text: qsTr("Choose files…")
                        onClicked: pageStack.push(filePickerComponent)
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Choose folder…")
                        onClicked: pageStack.push(Qt.resolvedUrl("FolderPickPage.qml"), {
                            startPath: StandardPaths.home,
                            pickedCallback: function(path) {
                                kelpLog.info("AddPage: folder picked \"" + path + "\"")
                                var info = kelpFiles.describe(path)
                                addPage.addEntry(path.split("/").pop(), path, "folder",
                                                 info.fileCount, info.totalBytes)
                            }
                        })
                    }
                }
            }

            SectionHeader { text: qsTr("Privacy") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Always encrypted. The key travels in the link, so only the people you send it to can read the bundle — the relay and any peers see only ciphertext.")
            }

            SectionHeader { text: qsTr("Serving") }

            TextSwitch {
                text: qsTr("Let peers help serve this bundle")
                description: addPage.store && addPage.store.swarmEnabled
                             ? qsTr("Other downloaders trade pieces among themselves. Faster for popular shares, but those peers see your IP address.")
                             : qsTr("Turn on “Allow peer swarm” in Settings to enable this.")
                checked: addPage.swarm
                enabled: addPage.store ? addPage.store.swarmEnabled : false
                onCheckedChanged: {
                    if (checked !== addPage.swarm)
                        addPage.swarm = checked
                }
            }

            Item {
                width: parent.width
                height: summaryColumn.height

                Column {
                    id: summaryColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Label {
                        width: parent.width
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        text: addPage.selection.length === 0
                              ? qsTr("Nothing to share yet")
                              : qsTr("%1 file(s) · %2").arg(addPage.selectedFileCount())
                                                       .arg(kelpFiles.formatSize(addPage.selectedBytes()))
                    }

                    Button {
                        width: parent.width
                        text: qsTr("Start sharing")
                        enabled: addPage.selection.length > 0 && !addPage.sealing
                        onClicked: addPage.startSharing()
                    }
                }
            }
        }
    }

    Toast { id: toast }

    Component {
        id: filePickerComponent

        FilePickerPage {
            // FilePickerPage defaults nameFilters to an empty string, which the
            // file model reads as a filter that matches nothing, so only
            // folders would show. "*" is the catch-all.
            nameFilters: [ "*" ]

            onSelectedContentPropertiesChanged: {
                var props = selectedContentProperties
                if (!props)
                    return
                kelpLog.info("AddPage: file picked \"" + props.filePath + "\"")
                var info = kelpFiles.describe(props.filePath)
                addPage.addEntry(props.fileName, props.filePath, "file",
                                 info.fileCount, info.totalBytes)
            }
        }
    }
}
