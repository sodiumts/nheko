// SPDX-FileCopyrightText: Nheko Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

import ".."
import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.3
import im.nheko 1.0

ApplicationWindow {
    id: inputDialog

    property alias prompt: promptLabel.text
    property alias echoMode: statusInput.echoMode
    property alias text: statusInput.text

    signal accepted(text: string)

    modality: Qt.NonModal
    flags: Qt.Dialog
    width: 350
    height: fontMetrics.lineSpacing * 7

    function forceActiveFocus() {
        statusInput.forceActiveFocus();
    }

    Shortcut {
        sequences: [StandardKey.Cancel]
        // We don't want this to steal the focus from other dialogs!
        // Workaround for https://qt-project.atlassian.net/browse/QTBUG-141691
        id: s
        enabled: Nheko.focusWindow == Nheko.findWindow(s)
        onActivated: dbb.rejected()
    }

    ColumnLayout {
        spacing: Nheko.paddingMedium
        anchors.margins: Nheko.paddingMedium
        anchors.fill: parent

        Label {
            id: promptLabel

            color: palette.text
        }

        MatrixTextField {
            id: statusInput

            Layout.fillWidth: true
            onAccepted: dbb.accepted()
            focus: true
        }

    }

    footer: DialogButtonBox {
        id: dbb

        standardButtons: DialogButtonBox.Ok | DialogButtonBox.Cancel
        onAccepted: {
            inputDialog.accepted(statusInput.text);

            inputDialog.close();
        }
        onRejected: {
            inputDialog.close();
        }
    }

}
