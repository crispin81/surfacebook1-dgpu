import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// Surface Book 1 dGPU: "󰢮 ON/off" in the bar, and a popup with the details
// and an on/off switch, so a stray click on the bar can't power it off.
// Switching runs `sudo -n /usr/local/bin/dgpu on|off` (needs a NOPASSWD rule).
Panel {
  id: root
  moduleName: "chriscork.dgpu"
  manageIpc: false

  property bool gpuOn: false
  property bool busy: false
  property bool wantOn: false
  property string details: ""
  property string errorText: ""

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property bool shownOn: busy ? wantOn : gpuOn

  function refresh() { if (!statusProc.running) statusProc.running = true }

  function setPower(on) {
    if (busy || on === gpuOn) return
    wantOn = on
    busy = true
    errorText = ""
    switchProc.command = ["sudo", "-n", "/usr/local/bin/dgpu", on ? "on" : "off"]
    switchProc.running = true
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: if (opened) refresh()

  // Prints "on" or "off", then nvidia-smi's "pstate, power, temp" when on.
  Process {
    id: statusProc
    command: ["bash", "-c",
      "if [ -e /sys/bus/pci/devices/0000:01:00.0 ]; then echo on; " +
      "nvidia-smi --query-gpu=pstate,power.draw,temperature.gpu --format=csv,noheader 2>/dev/null; " +
      "else echo off; fi"]
    stdout: StdioCollector {
      onStreamFinished: {
        var lines = String(text || "").trim().split("\n")
        root.gpuOn = lines[0] === "on"
        var f = root.gpuOn && lines.length > 1 ? lines[1].split(",") : []
        root.details = f.length === 3
          ? f[0].trim() + " · " + f[1].trim() + " · " + f[2].trim() + "°C" : ""
      }
    }
  }

  Process {
    id: switchProc
    stderr: StdioCollector { id: switchErr }
    onExited: function(exitCode) {
      root.busy = false
      if (exitCode !== 0) {
        var msg = String(switchErr.text || "").trim().split("\n").pop()
        root.errorText = msg !== "" ? msg : "dgpu exited with code " + exitCode
      }
      root.refresh()
    }
  }

  Timer {
    interval: root.opened ? 2000 : 3000
    running: true
    repeat: true
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: root.busy ? "󰢮 …" : (root.gpuOn ? "󰢮 ON" : "󰢮 off")
    active: root.gpuOn
    tooltipText: root.opened ? "" : (root.gpuOn ? "dGPU on" : "dGPU off")
    onPressed: function(buttonCode) { root.toggle() }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(320))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(240))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }
      onTextKey: function(t) { if (t === "r" || t === "R") root.refresh() }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(10)

        PanelHero {
          id: hero
          width: parent.width
          title: "NVIDIA dGPU"
          meta: root.busy ? (root.wantOn ? "Powering on…" : "Powering off…")
                          : (root.gpuOn ? "On" + (root.details ? " — " + root.details : "") : "Off")
          foreground: root.foreground
          fontFamily: root.fontFamily
          iconOpacity: root.shownOn ? 1.0 : 0.5
          iconComponent: Component {
            Text {
              text: "󰢮"
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
            }
          }
          trailingControl: Component {
            ToggleSwitch {
              id: powerSwitch
              checked: root.shownOn
              busy: root.busy
              foreground: hero.foreground
              onToggled: root.setPower(!root.gpuOn)

              PanelToolTip {
                visible: powerSwitch.containsMouse
                text: root.gpuOn ? "Power off the dGPU" : "Power on the dGPU"
                fontFamily: hero.fontFamily
              }
            }
          }
        }

        Text {
          textFormat: Text.PlainText
          visible: root.errorText !== ""
          width: parent.width
          text: root.errorText
          color: root.urgent
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          wrapMode: Text.WordWrap
        }

        Text {
          textFormat: Text.PlainText
          width: parent.width
          text: root.gpuOn
            ? "Powers off automatically when you press detach. Wait for the notification before lifting the screen."
            : "Run apps on it with prime-run or __NV_PRIME_RENDER_OFFLOAD=1."
          color: root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          wrapMode: Text.WordWrap
        }
      }
    }
  }
}
