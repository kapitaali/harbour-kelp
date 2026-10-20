import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"

/*
 * Open a kelp:// link someone sent you.
 *
 * The link is the whole capability: its fragment holds the key, and its query
 * holds the address to fetch from. Nothing is downloaded until the button is
 * pressed, and the preview below the field is read out of the link itself — no
 * network, so it answers instantly and works with no signal at all.
 *
 * There are two links, and they end differently. An ordinary one is *saved*:
 * "Download" fetches the objects, checks each against its address before keeping
 * it, and writes the files out under the chosen folder (Downloads by default).
 * A *webpage* link (design §5.5 — the link says so, in its sealed block) is
 * *viewed*: one "Open in browser" fetches the bundle into memory only, serves it
 * on 127.0.0.1, and hands that URL to the browser. Nothing is written, nothing is
 * remembered, and the memory goes with the session.
 *
 * Progress and the result come from kelpClient (C++), which owns both paths.
 */
Page {
    id: openPage

    // Named so the root's openLink() can recognise the page already on the
    // stack and replace its link instead of stacking another one.
    objectName: "openLinkPage"

    // The link as typed or pasted. Kept so a failed attempt can simply be
    // re-run, and so the preview has something to parse.
    property string link: ""

    // What the link says about itself. Recomputed as the text changes, and null
    // only while the field is empty. A link that does not parse comes back as
    // { ok: false, error } with nothing else, so it is filled out here: every
    // binding below can then read a field without asking whether it exists —
    // including the ones inside a hidden section, which QML still evaluates.
    property var parsed: {
        if (link.length === 0)
            return null
        var info = kelpBundles.linkInfo(link)
        return {
            "ok": info.ok === true,
            "error": info.error ? info.error : "",
            "bundleId": info.bundleId ? info.bundleId : "",
            "name": info.name ? info.name : "",
            "lan": info.lan ? info.lan : "",
            "relays": info.relays ? info.relays : [],
            // The publisher declared this a webpage (design §5.5): it is looked
            // at, never saved. A hint, not enforcement — but the ordinary client
            // does the right thing by default because of it.
            "page": info.page === true,
            // A passphrase link (design §12.3): the fragment holds the key wrapped
            // under a passphrase, not the key. Nothing but the content address and
            // the relay names can be read until one is given — the publisher sealed
            // the address, the label and the kind under that key.
            "needsPassphrase": info.needsPassphrase === true
        }
    }

    // The passphrase the recipient typed, and the link's own reading once it has
    // opened. `unlocked` starts null and is dropped whenever the link changes, so
    // a second link never inherits the first one's passphrase.
    property string passphrase: ""
    property var unlocked: null
    property string unlockError: ""

    // The one object every binding below reads — the unlocked reading once there
    // is one, the plain one until then — so opening a link does not mean every
    // binding learns a second source.
    property var facts: openPage.unlocked !== null ? openPage.unlocked : openPage.parsed

    // A passphrase link nobody has opened yet. There is nothing to fetch from and
    // nothing worth saying about the share, so the page asks for the passphrase
    // and shows nothing else.
    property bool locked: openPage.facts !== null && openPage.facts.ok
                          && openPage.facts.needsPassphrase === true

    onLinkChanged: {
        openPage.unlocked = null
        openPage.passphrase = ""
        openPage.unlockError = ""
    }

    property bool busy: kelpClient.busy
    property string phase: kelpClient.phase

    // What the busy row says. Two different questions, so two different answers:
    // a transfer this page did not start names the bundle it is waiting on
    // (§3.38) — `kelpClient.name` is the transfer in flight, and a link that
    // carried no name leaves the sentence as it was rather than showing an
    // identifier nobody can match — while this page's own transfer reports its
    // phase (§3.32).
    property string busyText: !kelpClient.isHandling(openPage.link)
                              ? (kelpClient.name.length > 0
                                 ? qsTr("Another transfer is still running: “%1”")
                                   .arg(kelpClient.name)
                                 : qsTr("Another transfer is still running…"))
                              : openPage.phase === "checking"
                                ? qsTr("Asking the sender…")
                                : openPage.phase === "saving"
                                  ? qsTr("Saving the files…")
                                  : kelpClient.total > 0
                                    ? qsTr("Fetched %1 of %2 objects")
                                      .arg(kelpClient.fetched).arg(kelpClient.total)
                                    : qsTr("Fetched %1 objects").arg(kelpClient.fetched)

    // A webpage link is a view, not a save: no folder, no Download, nothing
    // written. One action, and it is the browser's.
    property bool viewOnly: openPage.facts !== null && openPage.facts.ok
                            && openPage.facts.page

    // Set while "Open in browser" is fetching, so the page opens the moment the
    // bundle is complete — one press, two steps (design §5.5).
    property bool pendingOpen: false

    // What the view ended up serving, for the Done panel: the page that was shown,
    // and how many the bundle held (there is no picker, matching the sender's own
    // page — design §5.3).
    property string entry: ""
    property int pages: 0

    // This client outlives this page, so a page opened later can find the last
    // visit's finished fetch still sitting in it: a leftover "done" is the *last*
    // link's, not this one's. Asking the client which link it is handling is what
    // tells the two apart — without it, pressing Open in browser on a fresh link
    // re-opened the old session's URL instead of fetching (§3.25).
    property bool viewOpen: openPage.viewOnly && openPage.phase === "done"
                            && kelpClient.isHandling(openPage.link)
    property bool doneHere: openPage.phase === "done"
                            && (!openPage.viewOnly || openPage.viewOpen)
    // The same for a failure: the error panel must belong to this link too, or a
    // new link arrives wearing the last one's complaint.
    property bool failedHere: openPage.phase === "failed"
                              && kelpClient.isHandling(openPage.link)

    // The same question, asked the imperative way. A property-change handler — and
    // anything it calls — runs *before* a derived binding like viewOpen above has
    // been re-evaluated, so reading that property there answers for the state
    // before the change: that is how a finished view used to be re-handed out.
    // Everything imperative reads the client directly instead.
    function viewIsOpenNow() {
        return openPage.viewOnly && openPage.phase === "done"
               && kelpClient.isHandling(openPage.link)
    }

    // Fetching needs a link that parses and an address to fetch from: a link
    // carrying only a relay cannot be fetched by this version.
    property bool canFetch: openPage.facts !== null && openPage.facts.ok
                            && openPage.facts.lan.length > 0
                            && !openPage.locked && !busy

    // Where the files will be written. The chosen folder is remembered across
    // sessions; empty means the platform's Downloads folder, the default.
    property string downloadDirectory: kelpSettings.downloadDirectory
    property string downloadLabel: downloadDirectory.length > 0
                                   ? downloadDirectory : qsTr("Downloads")

    // The bundle arrived for viewing: hand it to the browser, if it was that
    // press that asked for it.
    onPhaseChanged: {
        if (openPage.viewIsOpenNow() && openPage.pendingOpen) {
            openPage.pendingOpen = false
            openPage.openInBrowser()
        }
    }

    function shortId(id) {
        return id.length > 16 ? id.substring(0, 16) + "…" : id
    }

    // The fragment is the decryption key — or, for a passphrase link, the key
    // wrapped under one — so it must never reach the log either way. The rest of
    // the link (relays, content address) is enough to debug with.
    function redactLink(text) {
        var hash = text.indexOf("#")
        return hash >= 0 ? text.substring(0, hash) + "#…" : text
    }

    /*
     * Opens a passphrase link with the passphrase the user typed (design §12.3).
     *
     * This is where the link stops being a bare content address: unwrapping the
     * key is what makes the publisher's hints readable, so the name, the address
     * and the kind all appear at once. Nothing has been asked of the network yet —
     * a wrong passphrase costs the derivation and not a round trip.
     *
     * The passphrase is not kept beyond this page: the fetch is given it again
     * when it starts, and the derivation is what proves it both times.
     */
    function unlockNow() {
        if (openPage.passphrase.length === 0) {
            openPage.unlockError = qsTr("Type the passphrase that came with the link.")
            return
        }

        kelpLog.info("OpenLinkPage: unlocking a passphrase link")
        var info = kelpBundles.unlockLink(openPage.link, openPage.passphrase)
        if (info.ok !== true) {
            // The derivation's own verdict, which is the only thing that can tell
            // a wrong passphrase from a damaged link.
            openPage.unlockError = info.error
            kelpLog.info("OpenLinkPage: unlock failed: " + info.error)
            return
        }

        openPage.unlockError = ""
        openPage.unlocked = info
        kelpLog.info("OpenLinkPage: unlocked " + shortId(info.bundleId) + " ("
                     + (info.name.length > 0 ? info.name : "unnamed") + ")"
                     + (info.page === true ? ", a webpage" : ""))
    }

    function startDownload() {
        kelpLog.info("OpenLinkPage: download pressed for \""
                     + redactLink(openPage.link) + "\"")
        if (!kelpClient.fetch(openPage.link, openPage.passphrase)) {
            kelpLog.error("OpenLinkPage: fetch refused: " + kelpClient.error)
            toast.show(qsTr("Could not start: %1").arg(kelpClient.error))
        }
    }

    // The whole of a webpage link's interaction: fetch it into memory, then hand
    // the served page to the browser. Pressing it again after the first success
    // re-opens the same session — it does not fetch, and it does not mint a
    // second URL (design §5.5, C4a2-plan.md D5).
    function startOpen() {
        if (openPage.viewIsOpenNow()) {
            openPage.openInBrowser()
            return
        }
        kelpLog.info("OpenLinkPage: open in browser pressed for \""
                     + redactLink(openPage.link) + "\"")
        if (!kelpClient.fetchForViewing(openPage.link, openPage.passphrase)) {
            kelpLog.error("OpenLinkPage: view refused: " + kelpClient.error)
            toast.show(qsTr("Could not start: %1").arg(kelpClient.error))
            return
        }
        openPage.pendingOpen = true
    }

    function openInBrowser() {
        var opened = kelpClient.openPageView()
        if (!opened.ok) {
            kelpLog.warn("OpenLinkPage: no page to open: " + opened.error)
            toast.show(opened.error.length > 0 ? opened.error
                                               : qsTr("Could not open the page"))
            return
        }
        // Logged without the URL, as the bundle page's own preview is: the address
        // carries the token that is the capability. The client writes the URL to
        // the log itself only in a build and a setting that ask for links.
        openPage.entry = opened.entry
        openPage.pages = opened.pages
        kelpLog.info("Preview: serving \"" + opened.entry
                     + "\" and handing it to the browser")
        var asked = Qt.openUrlExternally(opened.url)
        kelpLog.info("OpenLinkPage: browser asked for; Qt said "
                     + (asked ? "yes" : "no"))
        toast.show(qsTr("Opening in the browser"))
    }

    // Pick the folder to save into, reusing the Add page's chooser with the
    // wording that fits saving rather than sharing.
    function chooseFolder() {
        var start = downloadDirectory.length > 0 ? downloadDirectory
                                                 : StandardPaths.download
        pageStack.push(Qt.resolvedUrl("FolderPickPage.qml"), {
            startPath: start,
            title: qsTr("Save to"),
            hint: qsTr("Tap to open a folder · the button saves into the folder you are in"),
            confirmText: qsTr("Save here"),
            itemMenuText: qsTr("Save here"),
            pickedCallback: function(path) {
                kelpSettings.downloadDirectory = path
                kelpClient.directory = path
                kelpLog.info("OpenLinkPage: download folder set to \"" + path + "\"")
            }
        })
    }

    SilicaFlickable {
        id: flickable

        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column

            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Open a link") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Paste a kelp:// link you were sent. The key travels in the link, so anything you forward it to can read the bundle too.")
            }

            TextField {
                id: linkField

                width: parent.width
                label: qsTr("Link")
                placeholderText: "kelp://…"
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                                             | Qt.ImhUrlCharactersOnly
                text: openPage.link
                onTextChanged: openPage.link = text
            }

            Button {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: openPage.link.length === 0
                text: qsTr("Paste from clipboard")
                onClicked: {
                    linkField.text = Clipboard.text
                    kelpLog.info("OpenLinkPage: pasted " + Clipboard.text.length
                                 + " character(s) from the clipboard")
                }
            }

            // The link is malformed: nothing can be tried, so this is the answer.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: openPage.facts !== null && !openPage.facts.ok
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: openPage.facts !== null && !openPage.facts.ok
                      ? qsTr("That is not a link Kelp can read: %1").arg(openPage.facts.error)
                      : ""
            }

            // A passphrase link, before the passphrase. Nothing about the share is
            // known yet — not the name, not the address, not whether it is a page —
            // because all of it is sealed under the key the passphrase unwraps, so
            // asking for the passphrase is the only useful thing this page can do.
            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: openPage.locked

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.highlightColor
                    text: qsTr("This link is protected by a passphrase. The link on its own opens nothing; the sender will have given you the passphrase separately.")
                }

                TextField {
                    id: passphraseField

                    width: parent.width
                    label: qsTr("Passphrase")
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    text: openPage.passphrase
                    onTextChanged: openPage.passphrase = text
                    // Sailfish's TextField has no "accepted" signal, so there is
                    // no return-key submit to hook; the Unlock button below is
                    // the way in.
                }

                Button {
                    width: parent.width
                    enabled: openPage.passphrase.length > 0
                    text: qsTr("Unlock")
                    onClicked: openPage.unlockNow()
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: openPage.unlockError.length > 0
                    color: Theme.secondaryHighlightColor
                    font.pixelSize: Theme.fontSizeSmall
                    text: openPage.unlockError
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("Checking the passphrase takes a moment on purpose: it is what makes a guess at the link alone hopeless.")
                }
            }

            SectionHeader {
                visible: openPage.facts !== null && openPage.facts.ok && !openPage.locked
                text: qsTr("This link")
            }

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: openPage.facts !== null && openPage.facts.ok && !openPage.locked

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: openPage.facts && openPage.facts.name.length > 0
                          ? openPage.facts.name
                          : qsTr("Unnamed bundle")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("Content address %1").arg(openPage.facts
                                                         ? shortId(openPage.facts.bundleId) : "")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: openPage.facts && openPage.facts.lan.length > 0
                          ? qsTr("Fetching from %1").arg(openPage.facts.lan)
                          : qsTr("No address to fetch from yet — relays are not implemented")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: openPage.facts !== null && openPage.facts.relays.length > 0
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("The link also names a relay, which this version cannot use yet")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: openPage.viewOnly
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("This is a web page, shared to be looked at: it opens in your browser and nothing is saved on this device.")
                }
            }

            SectionHeader {
                visible: openPage.facts !== null && openPage.facts.ok
                         && !openPage.locked
                         && !openPage.busy && !openPage.doneHere
                         && !openPage.viewOnly
                text: qsTr("Save to")
            }

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: openPage.facts !== null && openPage.facts.ok
                         && !openPage.locked
                         && !openPage.busy && !openPage.doneHere
                         && !openPage.viewOnly

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    truncationMode: TruncationMode.Fade
                    text: openPage.downloadLabel
                }

                Button {
                    width: parent.width
                    text: qsTr("Choose folder…")
                    onClicked: openPage.chooseFolder()
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("A single file is saved into this folder; anything else becomes a folder named for the link.")
                }
            }

            Item {
                width: parent.width
                height: progressColumn.height

                Column {
                    id: progressColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Item {
                        width: parent.width
                        height: busyRow.height
                        visible: openPage.busy

                        Row {
                            id: busyRow
                            width: parent.width
                            spacing: Theme.paddingMedium

                            // The client is shared by every link, so "busy" can be
                            // someone else's transfer. Then nothing is happening
                            // *here*: no spinner, and no Cancel — that button
                            // would kill a transfer this page does not own — and
                            // the sentence names the bundle instead (§3.38).
                            BusyIndicator {
                                id: spinner
                                size: BusyIndicatorSize.Small
                                visible: kelpClient.isHandling(openPage.link)
                                running: visible
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Label {
                                width: busyRow.width
                                       - (spinner.visible ? spinner.width + busyRow.spacing : 0)
                                anchors.verticalCenter: parent.verticalCenter
                                wrapMode: Text.Wrap
                                text: openPage.busyText
                            }
                        }
                    }

                    Button {
                        width: parent.width
                        visible: openPage.busy && kelpClient.isHandling(openPage.link)
                        text: qsTr("Cancel")
                        onClicked: kelpClient.cancel()
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: openPage.failedHere
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: kelpClient.error
            }

            Item {
                width: parent.width
                height: doneColumn.height
                visible: openPage.doneHere

                Column {
                    id: doneColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.highlightColor
                        text: openPage.viewOnly ? qsTr("Opened in browser")
                                                : qsTr("Saved")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        visible: !openPage.viewOnly
                        text: qsTr("%1 file(s) · %2").arg(kelpClient.files)
                                                     .arg(kelpFiles.formatSize(kelpClient.bytes))
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        visible: openPage.viewOnly
                        text: qsTr("Nothing was saved — a page shared with you is only looked at.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: openPage.viewOnly && openPage.pages > 1
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        text: qsTr("The bundle holds %1 pages; the one shown is %2.")
                              .arg(openPage.pages).arg(openPage.entry)
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        visible: kelpClient.destination.length > 0
                        color: Theme.secondaryColor
                        font.pixelSize: Theme.fontSizeExtraSmall
                        text: kelpClient.destination
                    }
                }
            }

            Item {
                width: parent.width
                height: actionColumn.height

                Column {
                    id: actionColumn
                    anchors {
                        left: parent.left
                        right: parent.right
                        leftMargin: Theme.horizontalPageMargin
                        rightMargin: Theme.horizontalPageMargin
                    }
                    spacing: Theme.paddingMedium

                    Button {
                        width: parent.width
                        // A view keeps its button after it is done: pressing it
                        // again re-opens the same session.
                        visible: !openPage.busy && !openPage.locked
                                 && (openPage.phase !== "done" || openPage.viewOnly)
                        enabled: openPage.canFetch
                        text: openPage.viewOnly
                              ? qsTr("Open in browser")
                              : openPage.failedHere ? qsTr("Try again")
                                                    : qsTr("Download")
                        onClicked: openPage.viewOnly ? openPage.startOpen()
                                                     : openPage.startDownload()
                    }

                    Button {
                        width: parent.width
                        visible: openPage.doneHere
                        text: qsTr("Open another link")
                        onClicked: {
                            kelpClient.reset()
                            openPage.link = ""
                            linkField.text = ""
                        }
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: openPage.failedHere && kelpClient.destination.length > 0
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("What arrived before that is in %1").arg(kelpClient.destination)
            }
        }
    }

    Toast { id: toast }
}
