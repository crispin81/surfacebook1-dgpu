#!/bin/bash
# Remove surfacebook1-dgpu. Leaves the dGPU powered off.
set -uo pipefail
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

[[ -x /usr/local/bin/dgpu ]] && /usr/local/bin/dgpu off
systemctl stop dgpu-detach-watch.service 2>/dev/null
systemctl disable --now sb1-dock-thermal.service 2>/dev/null
rm -f /usr/local/bin/dgpu /usr/local/sbin/dgpu-detach-watch /etc/systemd/system/dgpu-detach-watch.service
rm -f /usr/local/sbin/sb1-dock-thermal /etc/systemd/system/sb1-dock-thermal.service
systemctl disable --now surface-webcam.service 2>/dev/null
rm -f /etc/systemd/system/surface-webcam.service /etc/modprobe.d/surface-webcam.conf \
  /etc/modules-load.d/surface-webcam.conf
systemctl daemon-reload
modprobe -r surfacebook1_dgpu_switch 2>/dev/null
for v in $(dkms status surfacebook1-dgpu 2>/dev/null | sed -n 's|^surfacebook1-dgpu/\([^,]*\),.*|\1|p' | sort -u); do
  dkms remove "surfacebook1-dgpu/$v" --all
  rm -rf "/usr/src/surfacebook1-dgpu-$v"
done
echo "Removed. (If you added /etc/sudoers.d/dgpu, delete it too.)"
