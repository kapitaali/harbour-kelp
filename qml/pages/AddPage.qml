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

    // Whether peers may help serve the shares made from this page. Off by default
    // and gated by the global "Allow peer swarm" permission in Settings.
    property bool swarm: false

    // True while a worker seals and publishes, so a button cannot be pressed
    // twice. Sealing runs off the GUI thread, so this actually paints.
    property bool sealing: kelpBundles.sealing

    // A passphrase link (design §12.3) is built before the buttons are pressed,
    // because pressing one seals and shares in the same gesture. Empty means an
    // ordinary link, whose key travels in its fragment.
    property string passphrase: protectSwitch.checked ? passphraseField.text : ""

    // The floor is the same one KelpBundles::seal enforces, so the UI never
    // offers a share that would be refused — but seal() is what decides.
    property bool passphraseOk: passphrase.length === 0 || passphrase.length >= 8

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
     * Starts sealing one thing. The work runs on a worker and the row is added by
     * the root page when it finishes, so a large folder can be left while it runs.
     * Returns false only when a seal is already in flight; failures arrive in
     * sealFinished() with a note shown there.
     */
    function shareNow(path, name, kind, page) {
        if (sealing) {
            kelpLog.info("AddPage: already sealing, ignoring " + path)
            return false
        }

        var peerSwarm = swarm && kelpSettings.peerSwarm
        // The passphrase never reaches kelpLog: what is logged is that there is
        // one, never what it is.
        var passphrase = addPage.passphrase
        kelpLog.info("AddPage: sharing " + kind + " \"" + name + "\" (" + path
                     + "), page=" + page + ", swarm=" + peerSwarm
                     + (passphrase.length > 0 ? ", passphrase-protected" : ""))

        if (!kelpBundles.sealAsync([ path ], name, kind, page, passphrase, peerSwarm)) {
            kelpLog.warn("AddPage: a seal is already running")
            return false
        }
        return true
    }

    // The worker's answer, for this page's own UI. The row itself is added by the
    // root's acceptSealed(), which is still there if this page was left.
    function sealFinished(info) {
        if (!info || info.ok !== true) {
            var why = info && info.error ? info.error : qsTr("unknown error")
            var what = info && info.name ? info.name : qsTr("that")
            // The passphrase is left in the field: the reason it failed may be
            // that it was too short, and retyping it is not the fix.
            kelpLog.error("AddPage: sealing \"" + what + "\" failed: " + why)
            toast.show(qsTr("Could not share %1: %2").arg(what).arg(why))
            return
        }

        // The passphrase has done its one job — the wrapped blob is what the row
        // keeps — so it is dropped rather than left in memory.
        passphraseField.text = ""

        kelpLog.info("AddPage: sealed \"" + info.name + "\" — " + info.objectCount
                     + " object(s), " + info.fileCount + " file(s), "
                     + info.totalBytes + " bytes, id=" + info.bundleId
                     + ", page=" + (info.page === true))
        // Symlinks are skipped rather than followed, so a link-heavy folder can
        // come out smaller than it looked.
        if (info.skippedLinks > 0)
            kelpLog.warn("AddPage: skipped " + info.skippedLinks
                         + " symbolic link(s) while sealing")

        leaving = true
    }

    Connections {
        target: kelpBundles

        onSealed: addPage.sealFinished(result)
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
        // "webpage", not "folder": the row's `page` flag is what decides what the
        // app offers (design §5.5), but this string is persisted and logged, so it
        // should not call a webpage share a folder (§3.26).
        return addPage.shareNow(path, path.split("/").pop(), "webpage", true)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Share") }

            SectionHeader { text: qsTr("Protect the link") }

            TextSwitch {
                id: protectSwitch

                text: qsTr("Require a passphrase")
                description: qsTr("The link alone will open nothing: send the passphrase separately.")
                checked: false
            }

            TextField {
                id: passphraseField

                visible: protectSwitch.checked
                width: parent.width
                label: qsTr("Passphrase")
                placeholderText: qsTr("At least 8 characters")
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                errorHighlight: text.length > 0 && text.length < 8
            }

            Label {
                visible: protectSwitch.checked
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Only someone with both the link and the passphrase can open this share, so send them apart. The passphrase is not stored anywhere: forget it and the link is locked for good — sharing the folder again mints a new one.")
            }

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
                        enabled: !addPage.sealing && addPage.passphraseOk
                        onClicked: pageStack.push(filePickerComponent)
                    }
                    Button {
                        width: parent.width
                        text: qsTr("Share folder…")
                        enabled: !addPage.sealing && addPage.passphraseOk
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
                        enabled: !addPage.sealing && addPage.passphraseOk
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

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: addPage.sealing
                visible: running
                size: BusyIndicatorSize.Medium
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: addPage.sealing
                text: qsTr("Sealing and sharing…")
                color: Theme.highlightColor
                font.pixelSize: Theme.fontSizeSmall
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
                visible: kelpSettings.shareInBackground === false
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Background serving is off in Settings, so this share is served only while Kelp is in front.")
            }

            // The serving policy can stop this share the moment it is made
            // (design §12.8), which is surprising unless it is said here first.
            Label {
                visible: kelpPolicy.block.length > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: kelpPolicy.block === "metered"
                      ? qsTr("Serving is stopped right now: this device's only connection is mobile data and “Wi-Fi only” is on. The share is still made and its link works as soon as Wi-Fi is back.")
                      : qsTr("Serving is stopped right now: the battery is low. The share is still made and its link works as soon as this device is charging.")
            }

            SectionHeader { text: qsTr("Privacy") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Always encrypted. The key travels in the link, so only the people you send it to can read the bundle — the relay and any peers see only ciphertext.")
                visible: addPage.passphrase.length === 0
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                visible: addPage.passphrase.length > 0
                text: qsTr("Always encrypted. The key does not travel in the link: it is wrapped under your passphrase, so someone needs both to read the bundle — and the relay and any peers still see only ciphertext.")
            }

            SectionHeader { text: qsTr("Serving") }

            TextSwitch {
                text: qsTr("Let peers help serve what I share here")
                description: kelpSettings.peerSwarm
                             ? qsTr("Not implemented yet — no share uses it. It would let other downloaders trade pieces among themselves, faster for popular shares, but those peers would see your IP address.")
                             : qsTr("Turn on “Allow peer swarm” in Settings to enable this. No share uses it yet.")
                checked: addPage.swarm
                enabled: kelpSettings.peerSwarm
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
