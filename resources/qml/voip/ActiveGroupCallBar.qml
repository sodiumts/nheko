import "../"
import QtQuick 2.15
import QtQuick.Controls 2.3
import QtQuick.Layouts 1.2
import im.nheko 1.0

Rectangle {
    id: callBar

    visible: MatrixRTCSession.isOnCall
    color: callInviteBar.color
    implicitHeight: visible ? rowLayout.height + 8 : 0
        
    property string elapsedTime: "00:00"

    RowLayout {
        id: rowLayout

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8


        Label {
            id: callStateLabel
            font.pointSize: fontMetrics.font.pointSize * 1.1
            color: "#000000"
            text: {
                switch (MatrixRTCSession.callState) {
                case Voip.CONNECTING: return qsTr("Connecting...")
                case Voip.CONNECTED: return callBar.elapsedTime
                default: return ""
                }
            }
        }

        Timer {
            id: callTimer
            property int startTime: 0
            interval: 1000
            running: MatrixRTCSession.callState === Voip.CONNECTED
            repeat: true

            onRunningChanged: {
                if (running) {
                    startTime = Math.floor(Date.now() / 1000)
                    callBar.elapsedTime = "00:00"
                } else {
                    startTime = 0
                    callBar.elapsedTime = "00:00"
                }
            }

            onTriggered: {
                let seconds = Math.floor(Date.now() / 1000 - startTime)
                let s = seconds % 60
                let m = Math.floor(seconds / 60) % 60
                let h = Math.floor(seconds / 3600)
                callBar.elapsedTime = (h ? (pad(h) + ":") : "") + pad(m) + ":" + pad(s)
            }

            function pad(n) { return (n < 10) ? ("0" + n) : n }
        }

        Item { Layout.fillWidth: true }

        Component {
            id: groupShareDialog

            GroupScreen {

            }
        }

        ImageButton {
            Layout.alignment: Qt.AlignBottom
            Layout.margins: 8
            buttonTextColor: "#000000"
            ToolTip.text: "Share screen."

            ToolTip.visible: hovered
            Layout.preferredHeight: 24
            Layout.preferredWidth: 24
            hoverEnabled: true
            visible: CallManager.callsSupported && showAllButtons && room.isInCall && room && room.roomMemberCount > 2
            opacity: 1
            image: ":/icons/icons/ui/screen-share.svg"
            onClicked: {
                if (room) {
                    var dialog = groupShareDialog.createObject(timelineRoot);
                    dialog.open();
                    timelineRoot.destroyOnClose(dialog)
                }
            }
        }

        ImageButton {
            Layout.preferredWidth: 24
            Layout.preferredHeight: 24
            buttonTextColor: "#000000"
            image: MatrixRTCSession.isMicMuted ? ":/icons/icons/ui/microphone-mute.svg" : ":/icons/icons/ui/microphone-unmute.svg"
            hoverEnabled: true
            ToolTip.visible: hovered
            ToolTip.text: MatrixRTCSession.isMicMuted ? qsTr("Unmute Mic") : qsTr("Mute Mic")
            onClicked: MatrixRTCSession.toggleMicMute()
        }
    }
}