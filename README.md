# headset-audio-switch [![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

I got tired of manually switching my desktop's audio output every time I turned my Arctis 7 on or off — so I built a thing that does it for me.

- **Headset turns on**  → audio switches to headset Game output
- **Headset turns off** → audio switches to the last used non-headset, non-HDMI sink
- **Manual switch**     → that sink becomes the new fallback

## Supported devices

| Model | Product ID |
|-------|-----------|
| Arctis 7 (2019) | `0x12ad` |
| Arctis 7 (original) | `0x1260` |
| Arctis 7+ | `0x220e` |
| Arctis Nova 7 | `0x2202` |
| Arctis Nova 7X | `0x2206` |

> **Disclaimer:** This has only been tested on my own Arctis 7 (2019 edition, `0x12ad`). Support for the other listed devices is theoretical — their product IDs are wired in and the HID protocol is compatible, but I haven't personally verified them.

## Build

```sh
make
```

Requires `libpulse` development headers (package: `libpulse` on Arch, `libpulse-dev` on Debian/Ubuntu).

## Install

```sh
sudo make install
```

This places the binary in `/usr/local/bin` and the udev rule in `/etc/udev/rules.d/`.
After install, reload udev:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Usage

```sh
headset-audio-switch [--interval SECONDS]
```

Runs in the foreground, watches headset power state every second, and switches audio automatically.

### As a systemd user service

```
# ~/.config/systemd/user/headset-audio-switch.service
[Unit]
Description=Auto-switch audio when headset is on/off

[Service]
ExecStart=/usr/local/bin/headset-audio-switch
Restart=on-failure

[Install]
WantedBy=default.target
```

```sh
systemctl --user enable --now headset-audio-switch
```

## How it works

Polls the headset battery via raw HID reports on `/dev/hidraw`. When battery > 0,
the headset is considered ON. Links `libpulse` directly.

## License

GPLv2. See [LICENSE](LICENSE).
