# surfacebook1-dgpu

Power the NVIDIA dGPU in a **Microsoft Surface Book 1** keyboard base on and
off from Linux, as often as you like, **without a reboot and without the
linux-surface kernel**.

- `dgpu on` / `dgpu off`: power the dGPU up (and load the NVIDIA driver) or
  shut it down completely, mid-session.
- **Detach safety**: while the dGPU is on, pressing the detach button powers it
  off automatically before you lift the screen.
- Optional status/toggle widget for the [Omarchy](https://omarchy.org) bar.

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
git clone https://github.com/<you>/surfacebook1-dgpu
cd surfacebook1-dgpu
sudo ./install.sh
dgpu on    # powers on, loads nvidia, prints the GPU
dgpu off
```

`install.sh` builds the module with DKMS (rebuilt automatically on kernel
updates) and installs `/usr/local/bin/dgpu`,
`/usr/local/sbin/dgpu-detach-watch` and its systemd unit.
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

- the screen battery (`BAT1`) starts discharging under the full load: several
  W, against about 0.08 W while docked or after simply unplugging the charger;
- the base battery (`BAT2`) may stop reporting a status at all.

Either condition has to hold for two samples in a row. Plugging and
unplugging the charger doesn't trigger it. Detection takes about half a
second on battery and up to about 4 seconds on the charger, so **wait for the
"dGPU powered off" notification before lifting the screen**.

If the dGPU is still in use, it sends a desktop notification instead of
powering off. Close the app before lifting the screen. The watcher only runs
while the dGPU is on.

After reattaching, run `dgpu on` again when you want the dGPU back.

### Passwordless toggle (optional)

To toggle from a bar or hotkey without a password prompt, allow just this
command (it is root-owned in `/usr/local/bin`, so it can't be edited to gain
root):

```sh
echo "$USER ALL=(root) NOPASSWD: /usr/local/bin/dgpu" | sudo tee /etc/sudoers.d/dgpu
sudo chmod 440 /etc/sudoers.d/dgpu && sudo visudo -c
```

### Omarchy bar widget (optional)

```sh
mkdir -p ~/.config/omarchy/bar/scripts
cp extras/omarchy-bar/dgpu-* ~/.config/omarchy/bar/scripts/
```

Then add this to `bar.layout.right` in `~/.config/omarchy/shell.json`:

```json
{ "id": "dgpu", "type": "command", "exec": "~/.config/omarchy/bar/scripts/dgpu-status",
  "interval": 3, "tooltip": "dGPU", "onClick": "~/.config/omarchy/bar/scripts/dgpu-toggle" }
```

It shows **󰢮 ON** (highlighted) or **󰢮 off**; clicking toggles. Needs the
sudoers rule above. The scripts print Waybar-style JSON, so they should also
work as a Waybar custom module.

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
