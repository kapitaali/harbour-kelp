import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    property var store

    // How many bundles really are being served, rather than how many there are:
    // a paused row has no address, so it is not counted here.
    function servedCount() {
        if (!cover.store)
            return 0
        var served = 0
        var model = cover.store.bundlesModel
        for (var i = 0; i < model.count; ++i)
            if (model.get(i).lan.length > 0)
                served++
        return served
    }

    // True while at least one share is not paused — i.e. the pause action has
    // something to silence. The revision read keeps callers of this live, since a
    // setProperty() on the model does not re-evaluate a plain get() by itself.
    function anyActive() {
        if (!cover.store)
            return false
        var model = cover.store.bundlesModel
        for (var i = 0; i < model.count; ++i)
            if (model.get(i).state !== "paused")
                return true
        return false
    }

    Column {
        anchors.centerIn: parent
        spacing: Theme.paddingSmall

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Kelp"
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeLarge
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeSmall
            text: (cover.store && cover.store.bundlesModel.count > 0)
                  ? qsTr("%n bundle(s)", "", cover.store.bundlesModel.count)
                  : qsTr("No bundles yet")
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            color: cover.servedCount() > 0 ? Theme.highlightColor : Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
            text: cover.servedCount() > 0
                  ? qsTr("%n bundle(s) served", "", cover.servedCount())
                  : qsTr("Nothing is being served")
        }
    }

    CoverActionList {
        CoverAction {
            // One action for the whole app: pause every share, or serve them all
            // again. The revision read makes the icon follow the real state.
            iconSource: (cover.store && cover.store.sharesRevision >= 0 && cover.anyActive())
                        ? "image://theme/icon-m-pause"
                        : "image://theme/icon-m-play"
            onTriggered: {
                if (!cover.store)
                    return
                if (cover.anyActive())
                    cover.store.pauseAllShares()
                else
                    cover.store.resumeAllShares()
            }
        }
    }
}
