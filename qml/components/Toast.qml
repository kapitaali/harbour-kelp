import QtQuick 2.0
import Sailfish.Silica 1.0

/*
 * Transient in-app feedback bubble.
 *
 * Deliberately not a Nemo.Notifications banner: those land in the system
 * events view, which would fill up on every copy/action.
 *
 * Usage: add `Toast { id: toast }` to a Page and call `toast.show("text")`.
 */
Rectangle {
    id: toast

    property string message: ""

    function show(text) {
        toast.message = text
        timer.restart()
    }

    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom
    anchors.bottomMargin: Theme.paddingLarge

    width: toastLabel.width + Theme.paddingLarge * 2
    height: toastLabel.height + Theme.paddingMedium

    radius: Theme.paddingSmall
    color: Theme.highlightBackgroundColor
    opacity: (timer.running && toast.message.length > 0) ? 1.0 : 0.0
    visible: opacity > 0.0

    Behavior on opacity {
        NumberAnimation { duration: 200 }
    }

    Label {
        id: toastLabel
        anchors.centerIn: parent
        text: toast.message
        color: Theme.primaryColor
        font.pixelSize: Theme.fontSizeSmall
    }

    Timer {
        id: timer
        interval: 1800
    }
}
