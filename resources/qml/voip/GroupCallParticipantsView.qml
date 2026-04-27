import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import im.nheko 1.0

Item {
    id: root

    property var participants: MatrixRTCSession.participants ?? []

    readonly property int maxColumns: 5
    readonly property int columns: Math.min(participants.length, maxColumns)

    GridLayout {
        anchors.fill: parent
        anchors.margins: 4
        columns: root.columns
        columnSpacing: 4
        rowSpacing: 4

        Repeater {
            model: participants

            delegate: Rectangle {
                id: cell
                color: palette.base
                border.color: palette.mid
                border.width: 1
                radius: 6

                Layout.fillWidth: true
                Layout.preferredHeight: avatarSize + nameHeight + 16
                Layout.maximumHeight: Layout.preferredHeight

                property string fullId: modelData
                property string baseId: fullId.split(':').slice(0,2).join(':')
                // somehow make this work across rooms so you can always click on the call view and see icons
                property string displayName: room ? room.memberDisplayName(baseId) : ""
                property string avatarSrc: room ? room.avatarUrl(baseId).replace("mxc://", "image://MxcImage/") : ""

                property real avatarSize: Math.min(cell.width * 0.6, 80)
                property real nameHeight: fontMetrics.lineSpacing * 1.2

                Rectangle {
                    anchors.fill: parent
                    radius: parent.radius
                    color: palette.highlight
                    opacity: ma.containsMouse ? 0.1 : 0.0
                    Behavior on opacity { NumberAnimation { duration: 150 } }
                }

                MouseArea {
                    id: ma
                    anchors.fill: parent
                    hoverEnabled: true
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 4

                    Avatar {
                        id: avatar
                        width: cell.avatarSize
                        height: cell.avatarSize
                        anchors.horizontalCenter: parent.horizontalCenter

                        userid: cell.baseId
                        displayName: cell.displayName
                        url: cell.avatarSrc
                        crop: true
                    }

                    Text {
                        id: nameText
                        width: parent.width
                        text: cell.displayName
                        color: palette.text
                        font.pointSize: 9
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        maximumLineCount: 1
                    }

                    HoverHandler { id: nameHover; target: nameText }

                    ToolTip {
                        visible: nameHover.hovered
                        text: cell.fullId
                        delay: 500
                    }
                }
            }
        }
    }
}