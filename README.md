# surfacebook1-dgpu

Power the NVIDIA dGPU in a **Microsoft Surface Book 1** keyboard base on and
off from Linux, as often as you like, **without a reboot and without the
linux-surface kernel**.

- `dgpu on` / `dgpu off`: power the dGPU up (and load the NVIDIA driver) or
  shut it down completely, mid-session.
- **Detach safety**: while the dGPU is on, pressing the detach button powers it
  off automatically before you lift the screen.
- Optional [Omarchy](https://omarchy.org) bar widget with an on/off switch
  (plus a plain Waybar-style status script).
- Optional: the **front camera as an ordinary webcam** for OBS, browsers and
  video calls (see [below](#front-camera-as-a-webcam-optional)).

> **Experimental.** This pokes firmware methods and PCIe root port registers.
> It works reliably on the machine it was developed on, but during
> development a half-finished power-on sequence once hung the system. Save
> your work before the first try.

## Why this exists

On the Surface Book 1, the firmware keeps the dGPU unpowered at boot and
exposes ACPI methods (`\_SB.PCI0.RP05.HGON` / `HGOF`) to switch it. Two
things stop that working on a normal kernel:

1. **`HGON`/`HGOF` talk to the Surface Aggregator (SAM) over I2C using
   `RawBytes` GenericSerialBus accesses**, which mainline `i2c-core-acpi`
   rejects (`protocol 0x0e not supported`). linux-surface patches the I2C
   core for this; the I2C core is built in, so that can't be done from a
   module. This driver instead wraps the I2C0 operation-region handler and
   handles RawBytes writes itself, passing everything else through.

2. **The firmware's `HGON` can't bring the link back after `HGOF`.** `HGOF`
   puts the root port into L2/L3 Ready. `HGON` then requests the L23 exit and
   waits 128 ms for it before re-enabling dynamic power gating (`DPGE`) and
   clearing `LEDM`, but on this hardware the exit only completes *after*
   those later steps. `HGON` gives up midway, the dGPU stays powered but
   invisible, and only a reboot recovers it. (This is likely the cause of the
   "only sporadically successful" reports on the linux-surface wiki.) The
   driver notices when `HGON` gave up and finishes the sequence.

The very first power-on after boot never hits problem 2, because the port has
not been through `HGOF` yet, which is why it can look like it works once.

The proprietary NVIDIA driver can't runtime-suspend Maxwell GPUs, so the dGPU
draws power the whole time it is on. Hence a manual switch.

## Tested on

| | |
|---|---|
| Device | Surface Book 1 (2015), base with GeForce GM108 (1 GB), UEFI 92.3748.768 |
| Kernel | `linux-omarchy` 7.2.8 (Arch); should work on any recent kernel with `CONFIG_KPROBES=y` |
| Driver | `nvidia-580xx-dkms` 580.178.04 (the 580 series is the last to support Maxwell) |

The 2016 Performance Base (GTX 965M-class) is **untested**. It probably uses
the same firmware methods; reports welcome.

## Install

Requirements: `dkms`, kernel headers for your kernel, and the NVIDIA 580xx
driver (e.g. `nvidia-580xx-dkms` + `nvidia-580xx-utils` on Arch).

```sh
git clone https://github.com/crispin81/surfacebook1-dgpu
cd surfacebook1-dgpu
sudo ./install.sh
dgpu on    # powers on, loads nvidia, prints the GPU
dgpu off
```

`install.sh` builds the module with DKMS (rebuilt automatically on kernel
updates) and installs `/usr/local/bin/dgpu`,
`/usr/local/sbin/dgpu-detach-watch` and its systemd unit, and enables
`sb1-dock-thermal` (see below).
`./uninstall.sh` removes everything.

If you already have older dGPU scripts or services (e.g. something that
re-powers the dGPU whenever it leaves the PCI bus), disable them; they will
fight `dgpu off`.

## Usage

```sh
dgpu on        # power on + load nvidia
dgpu off       # unload nvidia + power off (refuses while something uses it)
dgpu status    # on / off
```

Run something on the dGPU with PRIME render offload:

```sh
__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia glxinfo -B
# or `prime-run <app>` from the nvidia-prime package
```

### Detaching the screen

Linux gets **no detach event** on the Surface Book 1: the latch is handled by
the embedded controller, and nothing signals the OS. If the dGPU is powered
when the base comes off, apps using it crash and the NVIDIA driver may hang.

While the dGPU is on, `dgpu-detach-watch` polls the batteries twice a second
and runs `dgpu off` when it sees the latch open. Once the latch opens, the base
stops powering the screen, with or without the charger (which plugs into the
base):

- **The screen battery (`BAT1`) carries at least twice the base battery's
  (`BAT2`) load.** Docked on battery, both share the load but the base always
  carries more (at most 0.9 times as much on the screen side, measured under
  a 22 W load). Latched open, the base battery only feeds the base itself
  (about 2.5–3 W with the dGPU on) and the screen battery takes over the
  rest, 2–4 times as much. The screen battery's load on its own isn't enough:
  on battery it often carries several watts while docked.
- With the charger plugged in and the base battery charging, the screen
  battery discharging at all (held for 1.5 seconds, since a real unplug looks
  like that for about a second).
- The base battery may stop reporting a status at all.

Plugging and unplugging the charger doesn't trigger it, and nor does a heavy
load on battery. Detection takes about half a second, up to 2 seconds on the
charger, so **wait for the "dGPU powered off" notification before lifting the
screen**. If the base battery is almost empty, the firmware may shift the load
to the screen battery while docked, which would look like a detach: an idle
dGPU then gets switched off.

If the dGPU is still in use, it sends a desktop notification instead of
powering off. Close the app before lifting the screen. The watcher only runs
while the dGPU is on.

After reattaching, run `dgpu on` again when you want the dGPU back.

### No skin-temperature throttling while docked

With `thermald --adaptive` (the Arch default), thermald applies the
firmware's skin-temperature limits: trip points of 54-56 °C on the
clipboard's `TZ02`/`TZ03` sensors. Under sustained load, such as streaming
video for 20 minutes, the clipboard reaches them and thermald cuts the CPU's
power, even with the base attached and the machine sitting on a desk.

`sb1-dock-thermal` stops thermald while the base is attached and starts it
again when the latch opens, using the same battery test as
`dgpu-detach-watch`. It switches the limits back on after 2 seconds and off
only after 10 seconds of looking docked. Stopping the service starts thermald
again. The CPU's own throttling near its maximum temperature still applies
when thermald is off.

`install.sh` enables it, so it runs from boot. To keep the firmware limits
all the time instead:

```sh
sudo systemctl disable --now sb1-dock-thermal   # thermald starts again
journalctl -t sb1-dock-thermal                  # each switch is logged
```

### Front camera as a webcam (optional)

The Surface Book's cameras aren't USB webcams: they are raw sensors behind
Intel's IPU3, which only [libcamera](https://libcamera.org) can drive. Apps
that expect an ordinary webcam see a list of `ipu3-cio2` / `ipu3-imgu`
devices that show nothing but green. OBS's "Video Capture Device (PipeWire)"
source can reach libcamera, but it crashed or showed green with this camera's
NV12 output (OBS 32.2).

`webcam/` turns the front camera into an ordinary virtual webcam, **"Surface
Front Camera"** (`/dev/video20`, YUYV 4:2:2, 1280x720 at 30 fps), with
[v4l2-relayd](https://gitlab.com/vicamo/v4l2-relayd) feeding a v4l2loopback
device from libcamera. It runs **on demand**: the real camera, and its light,
switch on only while an app has the virtual camera open. The first second or
two after an app opens it is blank or green while the camera starts.

Tested on the linux-surface kernel (7.2.11), which has all three camera
drivers (`ov5693` front, `ov8865` rear, `ov7251` infrared).

```sh
sudo pacman -S --needed libcamera libcamera-ipa gst-plugin-libcamera gst-plugins-good v4l2loopback-dkms
yay -S v4l2-relayd            # AUR
sudo ./webcam/install.sh
```

Then pick "Surface Front Camera" in the app: in OBS, a **Video Capture Device
(V4L2)** source with YUYV 4:2:2 at 1280x720. Several apps can use it at once.
Don't pick "Built-in Front Camera" (the PipeWire/libcamera entry): while the
virtual webcam is in use, the relay holds the real camera. OBS's V4L2 source
logs "select timed out" many times a second while it streams; that's
harmless. `uninstall.sh` removes the webcam setup too.

### Passwordless toggle (optional)

To toggle from a bar or hotkey without a password prompt, allow just this
command (it is root-owned in `/usr/local/bin`, so it can't be edited to gain
root):

```sh
echo "$USER ALL=(root) NOPASSWD: /usr/local/bin/dgpu" | sudo tee /etc/sudoers.d/dgpu
sudo chmod 440 /etc/sudoers.d/dgpu && sudo visudo -c
```

### Omarchy bar widget (optional)

A bar widget showing **󰢮 ON** (highlighted) or **󰢮 off**. Clicking it opens a
popup with the dGPU's state, power draw and temperature and an on/off
switch, so a stray click on the bar can't power it off. Needs the sudoers
rule above.

```sh
cp -r extras/omarchy-plugin/chriscork.dgpu ~/.config/omarchy/plugins/
omarchy-shell shell rescanPlugins
omarchy plugin enable chriscork.dgpu right
```

### Waybar / command widget (optional)

`extras/waybar-command/` has a status script that prints Waybar-style JSON and
a click-to-toggle script (no confirmation). For a Waybar custom module, or an
Omarchy `"type": "command"` bar entry:

```json
{ "id": "dgpu", "type": "command", "exec": "~/.config/omarchy/bar/scripts/dgpu-status",
  "interval": 3, "tooltip": "dGPU", "onClick": "~/.config/omarchy/bar/scripts/dgpu-toggle" }
```

## Troubleshooting

- `dmesg | grep surfacebook1` should show `RawBytes handler installed on I2C0`
  at boot, and `completed HGON link bring-up` on a power-on after a power-off.
- `failed to run HGON: 4097` with `protocol 0x0e not supported` means the
  RawBytes handler isn't active (module not loaded, or `CONFIG_KPROBES`
  missing).
- `dGPU powered but not on the PCI bus` means please open an issue with the
  output of `sudo dmesg` and `sudo lspci -vvs 00:1c.0`.

## Credits and license

Based on the linux-surface `surfacebook1_dgpu_switch` driver and the
"i2c: acpi: Implement RawBytes read access" patch by
[Maximilian Luz](https://github.com/qzed), and the
[linux-surface wiki](https://github.com/linux-surface/linux-surface/wiki/Surface-Book).

GPL-2.0-or-later, see [LICENSE](LICENSE).
