import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    property var store

    // How many bundles really are being served, rather than what the (inert)
    // global switch says. The switch is a promise; this is the state.
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
            iconSource: (cover.store && cover.store.seedingEnabled)
                        ? "image://theme/icon-m-pause"
                        : "image://theme/icon-m-play"
            onTriggered: {
                if (cover.store)
                    cover.store.seedingEnabled = !cover.store.seedingEnabled
            }
        }
    }
}
