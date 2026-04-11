// SPDX-FileCopyrightText: Nheko Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

import "../"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import im.nheko

Popup {
    modal: true

    anchors.centerIn: parent;

    Component.onCompleted: {
        frameRateCombo.currentIndex = frameRateCombo.find(Settings.screenShareFrameRate);
    }
    Component.onDestruction: {
        MatrixRTCSession.closeScreenShare();
    }

    ColumnLayout {
        Label {
            Layout.topMargin: 16
            Layout.bottomMargin: 16
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.alignment: Qt.AlignLeft
            text: qsTr("Share desktop with %1?").arg(room.roomName)
            color: palette.windowText
        }

        RowLayout {
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8

            Label {
            Layout.alignment: Qt.AlignLeft
            text: qsTr("Method:")
            color: palette.windowText
            }

          ComboBox {
            id: screenshareType

            Layout.fillWidth: true
            model: MatrixRTCSession.screenShareTypeList()
            onCurrentIndexChanged: MatrixRTCSession.setScreenShareType(currentIndex);
          }
        }

        RowLayout {
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8

            Label {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Window:")
                color: palette.windowText
            }

            Button {
                visible: CallManager.screenShareType == Voip.XDP
                highlighted: !MatrixRTCSession.screenShareReady
                text: qsTr("Request screencast")
                onClicked: {
                  Settings.screenShareHideCursor = hideCursorCheckBox.checked;
                  MatrixRTCSession.setupScreenShareXDP();
                }
            }

        }

        RowLayout {
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8

            Label {
                Layout.alignment: Qt.AlignLeft
                text: qsTr("Frame rate:")
                color: palette.windowText
            }

            ComboBox {
                id: frameRateCombo

                Layout.fillWidth: true
                model: ["120", "90", "60", "50", "48", "30", "25", "20", "15", "10", "5", "2", "1"]
            }

        }

        GridLayout {
            columns: 2
            rowSpacing: 10
            Layout.margins: 8

            MatrixText {
                text: qsTr("Hide mouse cursor")
            }

            ToggleButton {
                id: hideCursorCheckBox

                Layout.alignment: Qt.AlignRight
                checked: Settings.screenShareHideCursor
            }

        }

        RowLayout {
            Layout.margins: 8

            Item {
                Layout.fillWidth: true
            }

            Button {
                visible: MatrixRTCSession.screenShareReady
                text: qsTr("Share")
                icon.source: "qrc:/icons/icons/ui/screen-share.svg"

                onClicked: {
                    Settings.screenShareFrameRate = frameRateCombo.currentText;
                    Settings.screenShareHideCursor = hideCursorCheckBox.checked;

                    MatrixRTCSession.startScreenshare(room.roomId, windowCombo.currentIndex);
                    close();
                }
            }

            Button {
                visible: CallManager.screenShareReady
                text: qsTr("Preview")
                onClicked: {
                    CallManager.previewWindow(windowCombo.currentIndex);
                }
            }

            Button {
                text: qsTr("Cancel")
                onClicked: {
                    close();
                }
            }

        }

    }

    background: Rectangle {
        color: palette.window
        border.color: palette.windowText
    }

}
