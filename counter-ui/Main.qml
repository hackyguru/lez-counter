// LEZ Counter — one number on the Logos Execution Zone testnet that everyone
// shares. Every copy of this module, on any machine, reads and bumps the same
// counter program; the core (lezcounter_core) does the wallet work and this
// file polls its state once a second.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root
    implicitWidth: 900
    implicitHeight: 720

    readonly property string coreId: "lezcounter_core"

    property var st: ({})
    property string lastError: ""
    property string copied: ""
    property bool showSettings: false

    readonly property bool ready: st.phase === "ready"
    readonly property var program: st.program || ({})
    readonly property var me: st.me || ({})
    readonly property var jobs: st.jobs || []
    readonly property var changes: st.changes || []
    readonly property var pending: jobs.filter(function (j) {
        return j.kind === "increment" && j.status !== "done" && j.status !== "failed";
    })
    readonly property int pendingTotal: pending.reduce(function (sum, j) {
        return sum + Number(j.label.substring(1));
    }, 0)

    // ── Logos bridge ─────────────────────────────────────────────────
    //   logos.callModule(id, method, [])            — synchronous, no-arg only
    //   logos.callModuleAsync(id, method, args, cb) — anything with arguments
    function call(method) {
        if (typeof logos === "undefined" || !logos.callModule)
            return null;
        return logos.callModule(coreId, method, []);
    }
    function callArgs(method, args, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            lastError = "The Logos bridge is unavailable.";
            return;
        }
        logos.callModuleAsync(coreId, method, args, function (raw) {
            var r = unwrap(raw, null);
            if (r && r.ok === false)
                lastError = r.error || "Something went wrong.";
            refresh();
            if (cb)
                cb(r);
        });
    }
    // The bridge JSON-encodes the module's return value, so a JSON-returning
    // method arrives double-encoded: parse while it's still a string.
    function unwrap(raw, def) {
        if (raw === null || raw === undefined)
            return def;
        var v = raw;
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v);
            } catch (e) {
                return (i === 0) ? def : v;
            }
        }
        return v;
    }
    function refresh() {
        var s = unwrap(call("state"), null);
        if (s && typeof s === "object") {
            st = s;
            for (var i = 0; i < jobs.length; ++i)
                if (jobs[i].status === "failed" && Date.now() - jobs[i].finished < 1500)
                    lastError = jobs[i].error;
        }
    }
    function bump(n) {
        lastError = "";
        callArgs("increment", [n]);
    }

    function fmt(n) {
        return Number(n || 0).toLocaleString(Qt.locale("en_US"), "f", 0);
    }
    function shortAddr(a) {
        a = a || "";
        return a.length > 14 ? a.substring(0, 6) + "…" + a.substring(a.length - 6) : a;
    }
    function whenOf(ms) {
        var s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
        if (s < 45)
            return "just now";
        if (s < 3600)
            return Math.max(1, Math.round(s / 60)) + " min ago";
        var d = new Date(ms);
        return (d.getHours() < 10 ? "0" : "") + d.getHours() + ":" + (d.getMinutes() < 10 ? "0" : "") + d.getMinutes();
    }

    TextEdit {
        id: clip
        visible: false
    }
    function copy(t, key) {
        clip.text = t;
        clip.selectAll();
        clip.copy();
        copied = key;
        copyTimer.restart();
    }
    Timer {
        id: copyTimer
        interval: 1600
        onTriggered: root.copied = ""
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Gradient {
        id: accentGrad
        GradientStop {
            position: 0.0
            color: "#F28E6B"
        }
        GradientStop {
            position: 1.0
            color: "#E1613A"
        }
    }

    component Bump: Rectangle {
        id: b
        property int by: 1
        property bool primary: false
        Layout.preferredWidth: primary ? 180 : 110
        implicitHeight: 64
        radius: 32
        gradient: primary ? accentGrad : null
        color: bMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundElevated
        border.color: primary ? "transparent" : Theme.palette.borderSecondary
        opacity: enabled ? 1 : 0.4
        scale: bMa.pressed ? 0.96 : 1
        Behavior on scale {
            NumberAnimation {
                duration: 80
            }
        }
        LogosText {
            anchors.centerIn: parent
            text: "+" + b.by
            color: b.primary ? "#ffffff" : Theme.palette.text
            font.pixelSize: b.primary ? 28 : 22
            font.weight: Theme.typography.weightBold
        }
        MouseArea {
            id: bMa
            anchors.fill: parent
            hoverEnabled: true
            enabled: b.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: root.bump(b.by)
        }
    }

    component Card: Rectangle {
        id: card
        default property alias content: inner.data
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 2 * Theme.spacing.xlarge
        color: Theme.palette.backgroundElevated
        radius: Theme.spacing.radiusXlarge
        border.color: Theme.palette.borderSecondary
        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.margins: Theme.spacing.xlarge
            spacing: Theme.spacing.medium
        }
    }

    component CopyRow: RowLayout {
        id: cr
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        spacing: Theme.spacing.medium
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            LogosText {
                text: cr.label
                color: Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
            }
            LogosText {
                Layout.fillWidth: true
                text: cr.value || "—"
                elide: Text.ElideMiddle
                font.family: "Menlo"
                color: Theme.palette.text
                font.pixelSize: Theme.typography.secondaryText
            }
        }
        LogosText {
            text: root.copied === cr.label ? "Copied ✓" : "Copy"
            visible: cr.value.length > 0
            color: Theme.palette.primary
            font.pixelSize: Theme.typography.secondaryText
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.copy(cr.value, cr.label)
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: Math.min(scroller.availableWidth - 2 * Theme.spacing.xlarge, 520)
            x: (scroller.availableWidth - width) / 2
            spacing: Theme.spacing.large

            // Header
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing.large
                spacing: Theme.spacing.medium
                LogosText {
                    Layout.fillWidth: true
                    text: "LEZ Counter"
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.subtitleText + 2
                    font.weight: Theme.typography.weightBold
                }
                Rectangle {
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: !root.ready ? Theme.palette.warning : root.st.network && root.st.network.online ? Theme.palette.success : Theme.palette.error
                }
                LogosText {
                    text: !root.ready ? (root.st.phase === "error" ? "Wallet error" : "Starting") : (root.st.network && root.st.network.online ? "Testnet · block " + root.st.network.block : "Offline")
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }

            // Error
            Rectangle {
                Layout.fillWidth: true
                visible: root.lastError.length > 0 || root.st.phase === "error"
                implicitHeight: errText.implicitHeight + 2 * Theme.spacing.medium
                radius: Theme.spacing.radiusLarge
                color: Theme.colors.getColor(Theme.palette.error, 0.14)
                LogosText {
                    id: errText
                    anchors.fill: parent
                    anchors.margins: Theme.spacing.medium
                    text: root.st.phase === "error" ? root.st.error : root.lastError
                    wrapMode: Text.WordWrap
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.secondaryText
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.lastError = ""
                    }
                }
            }

            // The number
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing.xxlarge
                spacing: 4
                LogosText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: "Everyone's counter"
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.primaryText
                }
                LogosText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: root.st.value === null || root.st.value === undefined ? "…" : root.fmt(root.st.value)
                    color: Theme.palette.text
                    font.pixelSize: 112
                    font.weight: Theme.typography.weightBold
                }
                LogosText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: root.pendingTotal > 0
                    text: "+" + root.pendingTotal + " on its way · usually under a minute"
                    color: Theme.palette.warning
                    font.pixelSize: Theme.typography.secondaryText
                }
                LogosText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: root.pendingTotal === 0 && !!root.program.configured
                    text: "Stored on the Logos Execution Zone — anyone with this module sees the same number"
                    wrapMode: Text.WordWrap
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.spacing.medium
                spacing: Theme.spacing.medium
                Bump {
                    by: 1
                    primary: true
                    enabled: root.ready && !!root.program.configured
                }
                Bump {
                    by: 5
                    enabled: root.ready && !!root.program.configured
                }
            }

            // Live changes
            Card {
                Layout.topMargin: Theme.spacing.large
                LogosText {
                    text: "Live"
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.subtitleText
                    font.weight: Theme.typography.weightBold
                }
                LogosText {
                    Layout.fillWidth: true
                    visible: root.changes.length === 0
                    text: "Bumps from you and everyone else show up here while the module is open."
                    wrapMode: Text.WordWrap
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                Repeater {
                    model: root.changes
                    delegate: RowLayout {
                        id: ch
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.spacing.medium
                        Rectangle {
                            implicitWidth: 32
                            implicitHeight: 32
                            radius: 16
                            color: ch.modelData.mine ? Theme.palette.primary : Theme.colors.getColor(Theme.palette.info, 0.8)
                            LogosText {
                                anchors.centerIn: parent
                                text: ch.modelData.mine ? "You" : "?"
                                color: "#ffffff"
                                font.pixelSize: ch.modelData.mine ? 10 : 14
                                font.weight: Theme.typography.weightBold
                            }
                        }
                        LogosText {
                            Layout.fillWidth: true
                            text: (ch.modelData.mine ? "You added " : "Someone added ") + ch.modelData.by
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.primaryText
                        }
                        LogosText {
                            text: "→ " + root.fmt(ch.modelData.value) + " · " + root.whenOf(ch.modelData.at)
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }
                }
            }

            // Settings
            LogosText {
                Layout.alignment: Qt.AlignHCenter
                text: root.showSettings ? "Hide details" : "Details"
                color: Theme.palette.primary
                font.pixelSize: Theme.typography.secondaryText
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showSettings = !root.showSettings
                }
            }
            Card {
                visible: root.showSettings
                CopyRow {
                    label: "Counter program"
                    value: root.program.address || ""
                }
                CopyRow {
                    label: "Counter account (where the number lives)"
                    value: root.program.counter || ""
                }
                CopyRow {
                    label: "Your wallet"
                    value: root.me.address || ""
                }
                RowLayout {
                    Layout.fillWidth: true
                    LogosText {
                        Layout.fillWidth: true
                        text: (root.me.lgo || "0") + " LGO for fees · topped up from the testnet faucet automatically"
                        wrapMode: Text.WordWrap
                        color: Theme.palette.textTertiary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                    LogosText {
                        text: "Get LGO"
                        color: Theme.palette.primary
                        font.pixelSize: Theme.typography.secondaryText
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.callArgs("getGas", [])
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Theme.palette.borderSecondary
                }
                LogosText {
                    Layout.fillWidth: true
                    text: "Use a different deployment (after a testnet reset, or your own copy). Leave empty for the built-in one."
                    wrapMode: Text.WordWrap
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    LogosTextField {
                        id: programField
                        Layout.fillWidth: true
                        implicitHeight: 40
                        placeholderText: "Program address"
                        text: root.program.custom ? root.program.address : ""
                    }
                    LogosButton {
                        implicitWidth: 80
                        implicitHeight: 40
                        text: "Use"
                        onClicked: root.callArgs("setProgram", [programField.text.trim()])
                    }
                }
            }

            Item {
                implicitHeight: Theme.spacing.xxlarge
            }
        }
    }
}
