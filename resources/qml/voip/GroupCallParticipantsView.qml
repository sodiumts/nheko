import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import org.freedesktop.gstreamer.Qt6GLVideoItem 1.0
import im.nheko 1.0

Item {
    id: root
    property var participants: MatrixRTCSession.participants ?? []
    readonly property int maxColumns: 5
    readonly property int columns: Math.min(participants.length, maxColumns)
    property var currentStreamingCell: null
    GridLayout {
        id: grid
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
                Layout.preferredHeight: isStreaming ? Math.max(cell.width * 9/16, avatarSize + nameHeight + 16) : avatarSize + nameHeight + 16
                Layout.maximumHeight: Layout.preferredHeight

                property string fullId: modelData
                property string baseId: fullId.split(':').slice(0,2).join(':')
                property string displayName: room ? room.memberDisplayName(baseId) : ""
                property string avatarSrc: room ? room.avatarUrl(baseId).replace("mxc://", "image://MxcImage/") : ""
                property real avatarSize: Math.min(cell.width * 0.6, 80)
                property real nameHeight: fontMetrics.lineSpacing * 1.2
                property real participantVolume: 1.0
                property bool isStreaming: MatrixRTCSession.streamingParticipants.includes(modelData)
                
                Component.onCompleted: {
                    participantVolume = MatrixRTCSession.getSavedParticipantVolume(fullId)
                }

                onIsStreamingChanged: {
                    if (isStreaming) {
                        root.currentStreamingCell = cell
                    } else if (root.currentStreamingCell === cell) {
                        root.currentStreamingCell = null
                    }
                }

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
                    acceptedButtons: Qt.RightButton

                    property real savedX: 0
                    property real savedY: 0

                    onClicked: (mouse) => {
                        savedX = mouse.x
                        savedY = mouse.y
                        volumePopup.open()
                    }
                }

                Popup {
                    id: volumePopup
                    width: 180
                    modal: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    padding: 10
                    
                    x: {
                        var xPos = ma.savedX
                        // Adjust if popup would go offscreen on the right
                        if (xPos + width > parent.width) {
                            xPos = parent.width - width - 4
                        }
                        // Adjust if popup would go offscreen on the left
                        if (xPos < 0) {
                            xPos = 4
                        }
                        return xPos
                    }
                    
                    y: {
                        var yPos = ma.savedY
                        // Adjust if popup would go offscreen at bottom
                        if (yPos + height > parent.height) {
                            yPos = parent.height - height - 4
                        }
                        // Adjust if popup would go offscreen at top
                        if (yPos < 0) {
                            yPos = 4
                        }
                        return yPos
                    }

                    background: Rectangle {
                        color: palette.window
                        border.color: palette.mid
                        border.width: 1
                        radius: 6
                    }

                    contentItem: Column {
                        width: parent.width
                        spacing: 6

                        Text {
                            text: "Volume: " + Math.round(cell.participantVolume * 100) + "%"
                            color: palette.text
                            font.pointSize: 8
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                        }

                        RowLayout {
                            width: parent.width
                            spacing: 6
                            ImageButton {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.margins: 8
                                buttonTextColor: cell.participantVolume === 0 ? "#ff3333" : "#000000"
                                ToolTip.text: cell.participantVolume === 0 ? "Unmute User" : "Mute User"

                                ToolTip.visible: hovered
                                Layout.preferredHeight: 24
                                Layout.preferredWidth: 24
                                hoverEnabled: true
                                visible: true
                                opacity: 1
                                image: cell.participantVolume === 0 ? 
                                    ":/icons/icons/ui/microphone-mute.svg"
                                    : ":/icons/icons/ui/microphone-unmute.svg"
                                
                                onClicked: {
                                    cell.participantVolume = cell.participantVolume === 0 ? 1.0 : 0.0
                                    MatrixRTCSession.setParticipantVolume(cell.fullId, cell.participantVolume)
                                }
                            }                            

                           Slider {
                                id: volumeSlider
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                from: 0.0
                                to: 2.0
                                stepSize: 0.01

                                Component.onCompleted: value = cell.participantVolume

                                Connections {
                                    target: cell
                                    function onParticipantVolumeChanged() {
                                        if (!volumeSlider.pressed)
                                            volumeSlider.value = cell.participantVolume
                                    }
                                }

                                onMoved: {
                                    cell.participantVolume = value
                                    MatrixRTCSession.setParticipantVolume(cell.fullId, value)
                                }

                                background: Rectangle {
                                    x: volumeSlider.leftPadding
                                    y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                                    implicitWidth: 200
                                    implicitHeight: 20
                                    width: volumeSlider.availableWidth
                                    height: 20
                                    radius: 2
                                    color: "transparent"

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: parent.width
                                        height: 4
                                        radius: 2
                                        color: palette.mid

                                        Rectangle {
                                            width: volumeSlider.visualPosition * parent.width
                                            height: parent.height
                                            radius: parent.radius
                                            color: palette.highlight
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        acceptedButtons: Qt.NoButton
                                    }
                                }

                                handle: Rectangle {
                                    implicitWidth: 20
                                    implicitHeight: 20
                                    x: volumeSlider.leftPadding + volumeSlider.visualPosition
                                    * (volumeSlider.availableWidth - width)
                                    y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                                    width: 20
                                    height: 20
                                    radius: 10
                                    color: volumeSlider.pressed ? palette.highlight : palette.button
                                    border.color: palette.mid
                                    border.width: 2
                                    
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.SizeHorCursor
                                        acceptedButtons: Qt.NoButton
                                    }
                                }
                            } 
                        }
                    }
                }

                Column {
                    visible: !cell.isStreaming
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

                        Image {
                            visible: cell.participantVolume === 0
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            Layout.alignment: Qt.AlignCenter
                            source: ":/icons/icons/ui/microphone-mute.svg"
                        }
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

    GstGLQt6VideoItem {
        id: globalVideoItem
        objectName: "groupCallVideoItem"
        x: root.currentStreamingCell ? grid.x + root.currentStreamingCell.x : 0
        y: root.currentStreamingCell ? grid.y + root.currentStreamingCell.y : 0
        width: root.currentStreamingCell ? root.currentStreamingCell.width : 0
        height: root.currentStreamingCell ? root.currentStreamingCell.height : 0
        opacity: root.currentStreamingCell ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        visible: opacity > 0
        z: 1
    }
}