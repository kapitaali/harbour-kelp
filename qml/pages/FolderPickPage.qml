import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.FileManager 1.0

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

        model: FileModel {
            id: fileModel

            path: pickPage.currentPath
            active: pickPage.status === PageStatus.Active
            includeFiles: false
            includeDirectories: true
            includeParentDirectory: true
            sortBy: FileModel.SortByName
            directorySort: FileModel.SortDirectoriesBeforeFiles
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
        }

        delegate: ListItem {
            id: row

            contentHeight: Theme.itemSizeSmall

            property bool isParent: model.baseName === ".." && model.extension === ""

            Label {
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * Theme.horizontalPageMargin
                text: row.isParent ? qsTr("Parent folder") : model.fileName
                truncationMode: TruncationMode.Elide
                color: row.highlighted ? Theme.highlightColor : Theme.primaryColor
            }

            onClicked: pickPage.currentPath = model.absolutePath

            menu: ContextMenu {
                MenuItem {
                    text: pickPage.itemMenuText
                    onClicked: pickPage.pick(model.absolutePath)
                }
            }
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
