import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    property var store

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

        // The store maintains these counters (see harbour-kelp.qml); the cover
        // only reads them. Walking the model with get() from inside a binding
        // re-enters the model while it evaluates, which QML reports as a binding
        // loop — that is why this is not a function call of its own.
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            color: (cover.store && cover.store.servedShares > 0)
                   ? Theme.highlightColor : Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
            text: (cover.store && cover.store.servedShares > 0)
                  ? qsTr("%n bundle(s) served", "", cover.store.servedShares)
                  : qsTr("Nothing is being served")
        }
    }

    CoverActionList {
        CoverAction {
            // One action for the whole app: pause every share, or serve them all
            // again — the same per-share pause/resume the list and Bundle page
            // offer. Something is active while at least one row is not paused.
            iconSource: (cover.store
                         && cover.store.bundlesModel.count - cover.store.pausedShares > 0)
                        ? "image://theme/icon-m-pause"
                        : "image://theme/icon-m-play"
            onTriggered: {
                if (!cover.store)
                    return
                if (cover.store.bundlesModel.count - cover.store.pausedShares > 0)
                    cover.store.pauseAllShares()
                else
                    cover.store.resumeAllShares()
            }
        }
    }
}
