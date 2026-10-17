import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0
import "../components"

/*
 * Share one thing (design §5.5).
 *
 * Three buttons, three kinds: a file, a folder, or a webpage — the folder that
 * holds a site. Each gesture picks and seals in one pass, so a share exists the
 * moment it is chosen and there is never a staging list to disagree with what is
 * on disk. The cost is that kinds cannot be mixed: sharing three files means
 * choosing three times, and each choice becomes its own bundle with its own link.
 *
 * "Share webpage…" is the one that asks a question before sealing: the chosen
 * folder must hold a page that the preview convention would find (§5.3, via
 * kelpFiles.pageEntry — the same Kelp::Web::chooseEntry the preview uses), so a
 * folder that holds none is refused here rather than shared as something the
 * recipient could not open.
 */
Page {
    id: addPage

    // Injected by BundlesPage.
    property var store

    // Whether peers may help serve the shares made from this page. Off by default
    // and gated by the global "Allow peer swarm" permission in Settings.
    property bool swarm: false

    // True while sealing, so a button cannot be pressed twice. Sealing blocks the
    // UI thread, so this never actually paints.
    property bool sealing: false

    // Set when something was sealed while a picker was on top: this page pops
    // itself as soon as it is on top again, so one gesture lands back in the list
    // with the new row visible, instead of on a page whose buttons are all spent.
    property bool leaving: false

    onStatusChanged: {
        if (status === PageStatus.Active && leaving) {
            leaving = false
            kelpLog.info("AddPage: leaving after sharing")
            // Not popped straight from here: popping re-enters this handler's
            // property while it is being evaluated, which QML reports as a binding
            // loop on `status`. The timer is what takes the pop out of that.
            leaveTimer.start()
        }
    }

    Timer {
        id: leaveTimer

        interval: 0
        repeat: false
        onTriggered: pageStack.pop()
    }

    /*
     * Seals one thing and adds it to the bundle list. Returns false — with a note
     * already shown — when it cannot be shared, in which case nothing was sealed
     * and this page stays put.
     */
    function shareNow(path, name, kind, page) {
        if (sealing) {
            kelpLog.info("AddPage: already sealing, ignoring " + path)
            return false
        }

        var peerSwarm = swarm && store && store.swarmEnabled
        kelpLog.info("AddPage: sharing " + kind + " \"" + name + "\" (" + path
                     + "), page=" + page + ", swarm=" + peerSwarm)

        sealing = true
        var sealed = kelpBundles.seal([ path ], name, page)
        sealing = false

        if (!sealed.ok) {
            kelpLog.error("AddPage: sealing \"" + name + "\" failed: " + sealed.error)
            toast.show(qsTr("Could not share %1: %2").arg(name).arg(sealed.error))
            return false
        }

        kelpLog.info("AddPage: sealed \"" + name + "\" — " + sealed.objectCount
                     + " object(s), " + sealed.fileCount + " file(s), "
                     + sealed.totalBytes + " bytes, id=" + sealed.bundleId
                     + ", page=" + (sealed.page === true))
        // Symlinks are skipped rather than followed, so a link-heavy folder can
        // come out smaller than it looked.
        if (sealed.skippedLinks > 0)
            kelpLog.warn("AddPage: skipped " + sealed.skippedLinks
                         + " symbolic link(s) while sealing")

        store.addBundle({
            "name": name,
            "kind": kind,
            "files": sealed.fileCount,
            "size": kelpFiles.formatSize(sealed.totalBytes),
            "path": path,
            "bytes": sealed.totalBytes,
            "swarm": peerSwarm,
            "page": sealed.page === true,
            "lan": sealed.lan,
            "link": sealed.link,
            "bundleId": sealed.bundleId,
            "secret": sealed.secret,
            "objects": sealed.objectCount
        })

        leaving = true
        return true
    }

    // Share webpage: only a folder that really holds a page. The check is a look
    // at names, not contents — enough to refuse a folder that would arrive as
    // something unopenable, and not a promise that the page is any good.
    function shareWebpage(path) {
        var info = kelpFiles.pageEntry(path)
        if (!info.ok) {
            kelpLog.warn("AddPage: cannot look for a page in \"" + path + "\": " + info.error)
            toast.show(info.error)
            return false
        }
        if (!info.hasPage) {
            kelpLog.info("AddPage: \"" + path + "\" holds no page")
            toast.show(qsTr("That folder holds no page — no index.html or other HTML file. Share it as a folder instead."))
            return false
        }
        kelpLog.info("AddPage: \"" + path + "\" offers the page " + info.entry
                     + " (of " + info.candidates.length + " HTML file(s))")
        return addPage.shareNow(path, path.split("/").pop(), "folder", true)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Share") }

            SectionHeader { text: qsTr("What to share") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("One choice, one share, one link. Sharing several things means choosing several times.")
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
                        text: qsTr("Share file…")
                        enabled: !addPage.sealing
                        onClicked: pageStack.push(filePickerComponent)
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Share folder…")
                        enabled: !addPage.sealing
                        onClicked: pageStack.push(Qt.resolvedUrl("FolderPickPage.qml"), {
                            startPath: StandardPaths.home,
                            pickedCallback: function(path) {
                                kelpLog.info("AddPage: folder picked \"" + path + "\"")
                                addPage.shareNow(path, path.split("/").pop(), "folder", false)
                            }
                        })
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Share webpage…")
                        enabled: !addPage.sealing
                        onClicked: pageStack.push(Qt.resolvedUrl("FolderPickPage.qml"), {
                            startPath: StandardPaths.home,
                            title: qsTr("Choose webpage folder"),
                            hint: qsTr("Tap to open · long-press a folder to share it directly"),
                            confirmText: qsTr("Share this webpage"),
                            itemMenuText: qsTr("Share this webpage"),
                            pickedCallback: function(path) {
                                kelpLog.info("AddPage: webpage folder picked \"" + path + "\"")
                                addPage.shareWebpage(path)
                            }
                        })
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("A folder is shared whole. A webpage is the folder that holds a site — its index.html and whatever that page uses — and the link then says so, so a recipient that understands it opens the page in a browser instead of unpacking a copy.")
            }

            Label {
                visible: addPage.store && addPage.store.seedingEnabled === false
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Background serving is off in Settings, so this share is served only while Kelp is in front.")
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
                text: qsTr("Let peers help serve what I share here")
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

            // One file per gesture: the first selection is sealed and both this
            // picker and the Share page step aside. Picking several at once would
            // mean minting several links from one tap, which is not what the
            // button says, so the picker is dismissed after the first.
            onSelectedContentPropertiesChanged: {
                var props = selectedContentProperties
                if (!props)
                    return
                kelpLog.info("AddPage: file picked \"" + props.filePath + "\"")
                if (addPage.shareNow(props.filePath, props.fileName, "file", false))
                    pageStack.pop()
            }
        }
    }
}
