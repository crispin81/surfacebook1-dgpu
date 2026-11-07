#!/bin/bash
# Surface Book 1 front camera as an on-demand virtual webcam ("Surface Front
# Camera", /dev/video20). v4l2-relayd runs the libcamera pipeline only while
# an app has the virtual camera open, so the real camera (and its light) is
# off otherwise.
set -euo pipefail
cd "$(dirname "$0")"
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

missing=()
for pkg in libcamera libcamera-ipa gst-plugin-libcamera gst-plugins-good v4l2loopback-dkms v4l2-relayd; do
  pacman -Q "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if ((${#missing[@]})); then
  echo "Missing packages: ${missing[*]}" >&2
  echo "Install them first (v4l2-relayd is in the AUR):" >&2
  echo "  sudo pacman -S --needed libcamera libcamera-ipa gst-plugin-libcamera gst-plugins-good v4l2loopback-dkms" >&2
  echo "  yay -S v4l2-relayd" >&2
  exit 1
fi
if ! lsmod | grep -q '^ov5693'; then
  echo "The front camera's sensor driver (ov5693) isn't loaded." >&2
  echo "This was tested on the linux-surface kernel; check that your kernel has" >&2
  echo "the IPU3 and ov5693 drivers and that 'cam -l' lists the front camera." >&2
  exit 1
fi

install -Dm644 modprobe.conf /etc/modprobe.d/surface-webcam.conf
install -Dm644 modules-load.conf /etc/modules-load.d/surface-webcam.conf
install -Dm644 surface-webcam.service /etc/systemd/system/surface-webcam.service

# Reload the module with these options if it was already loaded
modprobe -r v4l2loopback 2>/dev/null || true
modprobe v4l2loopback
systemctl daemon-reload
systemctl enable --now surface-webcam.service

echo
echo "Virtual webcam: /dev/video20 ($(cat /sys/devices/virtual/video4linux/video20/name 2>/dev/null))"
echo "Pick \"Surface Front Camera\" in your app (OBS: Video Capture Device (V4L2), YUYV 4:2:2, 1280x720)."
