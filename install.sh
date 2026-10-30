#!/bin/bash
# Install surfacebook1-dgpu: DKMS module, dgpu command, detach watcher.
set -euo pipefail
cd "$(dirname "$0")"
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"

VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' module/dkms.conf)

if [[ $(cat /sys/class/dmi/id/product_name 2>/dev/null) != "Surface Book" ]]; then
  echo "This is for the Surface Book 1 (DMI product name \"Surface Book\")." >&2
  read -rp "Continue anyway? [y/N] " ans; [[ $ans == [yY] ]] || exit 1
fi
command -v dkms >/dev/null || { echo "dkms is required." >&2; exit 1; }

# Replace any previously installed version.
for v in $(dkms status surfacebook1-dgpu 2>/dev/null | sed -n 's|^surfacebook1-dgpu/\([^,]*\),.*|\1|p' | sort -u); do
  dkms remove "surfacebook1-dgpu/$v" --all || true
  rm -rf "/usr/src/surfacebook1-dgpu-$v"
done
modprobe -r surfacebook1_dgpu_switch 2>/dev/null || true

mkdir -p "/usr/src/surfacebook1-dgpu-$VERSION"
cp module/surfacebook1_dgpu_switch.c module/Makefile module/dkms.conf "/usr/src/surfacebook1-dgpu-$VERSION/"
dkms install "surfacebook1-dgpu/$VERSION"
modprobe surfacebook1_dgpu_switch

install -Dm755 bin/dgpu /usr/local/bin/dgpu
install -Dm755 sbin/dgpu-detach-watch /usr/local/sbin/dgpu-detach-watch
install -Dm644 systemd/dgpu-detach-watch.service /etc/systemd/system/dgpu-detach-watch.service
systemctl daemon-reload

echo
echo "Installed surfacebook1-dgpu $VERSION. Try: dgpu on && dgpu off"
