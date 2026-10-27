import QtQuick 2.0
import Sailfish.Silica 1.0
import Qt.labs.folderlistmodel 2.1

/*
 * Folder chooser.
 *
 * The packaged FolderPickerPage selects the current folder with a blank button
 * in its dialog header, which is easy to miss. Here tapping a folder opens it,
 * and the folder you are in is always shared with the button pinned at the
 * bottom of the screen — so "choose this folder" is never hidden behind a
 * gesture. Long-pressing a folder chooses that subfolder without opening it.
 *
 * The wording is configurable so the same page serves both "pick a folder to
 * share" (the Add page) and "pick a folder to save into" (Open a link); the
 * defaults keep the sharing wording.
 *
 * Qt.labs.folderlistmodel is on Harbour's allowed list; Nemo.FileManager,
 * which the packaged picker uses internally, is not importable from an app.
 */
Page {
    id: pickPage

    // Injected by the caller.
    property string startPath: StandardPaths.home
    property var pickedCallback     // function(path)

    // Wording; overridden by the Open-a-link page when picking a save folder.
    property string title: qsTr("Choose folder")
    property string hint: qsTr("Tap to open · long-press a folder to share it directly")
    property string confirmText: qsTr("Share this folder")
    property string itemMenuText: qsTr("Share this folder")

    property string currentPath: startPath

    // The row above the model: the folder that holds this one. Root has none.
    property string parentPath: {
        var path = currentPath.replace(/\/+$/, "")
        var cut = path.lastIndexOf("/")
        if (cut <= 0)
            return "/"
        return path.substring(0, cut)
    }

    function pick(path) {
        kelpLog.info("FolderPickPage: picked \"" + path + "\"")
        if (pickedCallback)
            pickedCallback(path)
        pageStack.pop()
    }

    SilicaListView {
        id: listView

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: shareButton.top
            bottomMargin: Theme.paddingMedium
        }

        header: Column {
            width: listView.width

            PageHeader { title: pickPage.title }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: pickPage.currentPath
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: pickPage.hint
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
            }

            ListItem {
                id: parentRow

                contentHeight: Theme.itemSizeSmall
                visible: pickPage.currentPath !== "/"

                Label {
                    x: Theme.horizontalPageMargin
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 2 * Theme.horizontalPageMargin
                    text: qsTr("Parent folder")
                    color: parentRow.highlighted ? Theme.highlightColor : Theme.primaryColor
                }

                onClicked: pickPage.currentPath = pickPage.parentPath
            }
        }

        model: FolderListModel {
            id: folderModel

            folder: "file://" + pickPage.currentPath
            showDirs: true
            showFiles: false
            showDotAndDotDot: false
            sortField: FolderListModel.Name
        }

        delegate: ListItem {
            id: row

            contentHeight: Theme.itemSizeSmall

            Label {
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: model.fileName
                truncationMode: TruncationMode.Elide
                color: row.highlighted ? Theme.highlightColor : Theme.primaryColor
            }

            onClicked: pickPage.currentPath = model.filePath

            menu: ContextMenu {
                MenuItem {
                    text: pickPage.itemMenuText
                    onClicked: pickPage.pick(model.filePath)
                }
            }
        }

        ViewPlaceholder {
            text: qsTr("No subfolders")
            enabled: listView.count === 0
            hintText: qsTr("Tap the button below to choose this folder")
        }

        VerticalScrollDecorator {}
    }

    // Pinned, not scrolled: choosing a folder is the entire purpose of this
    // page, so it must not depend on the user discovering a gesture.
    Button {
        id: shareButton

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            leftMargin: Theme.horizontalPageMargin
            rightMargin: Theme.horizontalPageMargin
            bottomMargin: Theme.paddingLarge
        }

        text: pickPage.confirmText
        onClicked: pickPage.pick(pickPage.currentPath)
    }
}
