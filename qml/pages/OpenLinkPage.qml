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
            "page": info.page === true
        }
    }

    property bool busy: kelpClient.busy
    property string phase: kelpClient.phase

    // A webpage link is a view, not a save: no folder, no Download, nothing
    // written. One action, and it is the browser's.
    property bool viewOnly: parsed !== null && parsed.ok && parsed.page

    // Set while "Open in browser" is fetching, so the page opens the moment the
    // bundle is complete — one press, two steps (design §5.5).
    property bool pendingOpen: false

    // What the view ended up serving, for the Done panel: the page that was shown,
    // and how many the bundle held (there is no picker, matching the sender's own
    // page — design §5.3).
    property string entry: ""
    property int pages: 0

    // Fetching needs a link that parses and an address to fetch from: a link
    // carrying only a relay cannot be fetched by this version.
    property bool canFetch: parsed !== null && parsed.ok && parsed.lan.length > 0
                            && !busy

    // Where the files will be written. The chosen folder is remembered across
    // sessions; empty means the platform's Downloads folder, the default.
    property string downloadDirectory: kelpSettings.downloadDirectory
    property string downloadLabel: downloadDirectory.length > 0
                                   ? downloadDirectory : qsTr("Downloads")

    // The bundle arrived for viewing: hand it to the browser, if it was that
    // press that asked for it.
    onPhaseChanged: {
        if (openPage.viewOnly && openPage.phase === "done" && openPage.pendingOpen) {
            openPage.pendingOpen = false
            openPage.openInBrowser()
        }
    }

    function shortId(id) {
        return id.length > 16 ? id.substring(0, 16) + "…" : id
    }

    // The fragment is the decryption key, so it must never reach the log. The
    // rest of the link (address, name, content address) is enough to debug with.
    function redactLink(text) {
        var hash = text.indexOf("#")
        return hash >= 0 ? text.substring(0, hash) + "#k=…" : text
    }

    function startDownload() {
        kelpLog.info("OpenLinkPage: download pressed for \""
                     + redactLink(openPage.link) + "\"")
        if (!kelpClient.fetch(openPage.link)) {
            kelpLog.error("OpenLinkPage: fetch refused: " + kelpClient.error)
            toast.show(qsTr("Could not start: %1").arg(kelpClient.error))
        }
    }

    // The whole of a webpage link's interaction: fetch it into memory, then hand
    // the served page to the browser. Pressing it again after the first success
    // re-opens the same session — it does not fetch, and it does not mint a
    // second URL (design §5.5, C4a2-plan.md D5).
    function startOpen() {
        if (openPage.viewOnly && openPage.phase === "done") {
            openPage.openInBrowser()
            return
        }
        kelpLog.info("OpenLinkPage: open in browser pressed for \""
                     + redactLink(openPage.link) + "\"")
        if (!kelpClient.fetchForViewing(openPage.link)) {
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
                visible: openPage.parsed !== null && !openPage.parsed.ok
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: openPage.parsed !== null && !openPage.parsed.ok
                      ? qsTr("That is not a link Kelp can read: %1").arg(openPage.parsed.error)
                      : ""
            }

            SectionHeader {
                visible: openPage.parsed !== null && openPage.parsed.ok
                text: qsTr("This link")
            }

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: openPage.parsed !== null && openPage.parsed.ok

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: openPage.parsed && openPage.parsed.name.length > 0
                          ? openPage.parsed.name
                          : qsTr("Unnamed bundle")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: qsTr("Content address %1").arg(openPage.parsed
                                                         ? shortId(openPage.parsed.bundleId) : "")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    color: Theme.secondaryColor
                    font.pixelSize: Theme.fontSizeExtraSmall
                    text: openPage.parsed && openPage.parsed.lan.length > 0
                          ? qsTr("Fetching from %1").arg(openPage.parsed.lan)
                          : qsTr("No address to fetch from yet — relays are not implemented")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    visible: openPage.parsed !== null && openPage.parsed.relays.length > 0
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
                visible: openPage.parsed !== null && openPage.parsed.ok
                         && !openPage.busy && openPage.phase !== "done"
                         && !openPage.viewOnly
                text: qsTr("Save to")
            }

            Column {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingSmall
                visible: openPage.parsed !== null && openPage.parsed.ok
                         && !openPage.busy && openPage.phase !== "done"
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

                            BusyIndicator {
                                id: spinner
                                size: BusyIndicatorSize.Small
                                running: openPage.busy
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Label {
                                width: busyRow.width - spinner.width - busyRow.spacing
                                anchors.verticalCenter: parent.verticalCenter
                                wrapMode: Text.Wrap
                                text: openPage.phase === "checking"
                                      ? qsTr("Asking the sender…")
                                      : openPage.phase === "saving"
                                        ? qsTr("Saving the files…")
                                        : kelpClient.total > 0
                                          ? qsTr("Fetched %1 of %2 objects")
                                            .arg(kelpClient.fetched).arg(kelpClient.total)
                                          : qsTr("Fetched %1 objects").arg(kelpClient.fetched)
                            }
                        }
                    }

                    Button {
                        width: parent.width
                        visible: openPage.busy
                        text: qsTr("Cancel")
                        onClicked: kelpClient.cancel()
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: openPage.phase === "failed"
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: kelpClient.error
            }

            Item {
                width: parent.width
                height: doneColumn.height
                visible: openPage.phase === "done"

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
                        visible: !openPage.busy
                                 && (openPage.phase !== "done" || openPage.viewOnly)
                        enabled: openPage.canFetch
                        text: openPage.viewOnly
                              ? qsTr("Open in browser")
                              : openPage.phase === "failed" ? qsTr("Try again")
                                                            : qsTr("Download")
                        onClicked: openPage.viewOnly ? openPage.startOpen()
                                                     : openPage.startDownload()
                    }

                    Button {
                        width: parent.width
                        visible: openPage.phase === "done"
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
                visible: openPage.phase === "failed" && kelpClient.destination.length > 0
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("What arrived before that is in %1").arg(kelpClient.destination)
            }
        }
    }

    Toast { id: toast }
}
