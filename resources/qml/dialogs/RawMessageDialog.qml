// SPDX-FileCopyrightText: Nheko Contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import im.nheko

ApplicationWindow {
    id: rawMessageRoot

    property alias rawMessage: rawMessageView.text

    height: 420
    width: 420
    color: palette.window
    flags: Qt.Tool | Qt.WindowStaysOnTopHint | Qt.WindowCloseButtonHint | Qt.WindowTitleHint

    Shortcut {
        sequences: [StandardKey.Cancel]
        // We don't want this to steal the focus from other dialogs!
        // Workaround for https://qt-project.atlassian.net/browse/QTBUG-141691
        id: s
        enabled: Nheko.focusWindow == Nheko.findWindow(s)
        onActivated: rawMessageRoot.close()
    }

    ScrollView {
        anchors.margins: Nheko.paddingMedium
        anchors.fill: parent
        padding: Nheko.paddingMedium

        TextArea {
            id: rawMessageView

            font: Nheko.monospaceFont()
            color: palette.text
            readOnly: true
            textFormat: Text.PlainText

            anchors.fill: parent

            background: Rectangle {
                color: palette.base
            }

        }

    }

    footer: DialogButtonBox {
        standardButtons: DialogButtonBox.Ok
        onAccepted: rawMessageRoot.close()
    }

}
