# Glint

Mirror your iPad (or iPhone) screen to a Linux PC over Wi-Fi, using AirPlay.

Run `glint` on the PC, then on the iPad open **Control Center → Screen Mirroring** and pick your PC. The iPad screen appears in a window on the PC, and its sound plays through the PC speakers. Nothing needs to be installed on the iPad.

## Features

- **No app on the iPad**: the PC shows up in the standard Screen Mirroring list, like an Apple TV.
- **Video** up to 1080p at 60 fps, decoded with FFmpeg, with low latency.
- **Audio** plays on the PC; the iPad's volume buttons control it.
- **Accurate colors**: the window uses the color format the iPad actually sends (full-range BT.709).
- **Window follows the iPad**: it resizes when you rotate the iPad, and has a fullscreen mode.
- **Recovers on its own** when the Wi-Fi connection drops.

## How it works

```
iPad ── mDNS ──▶ finds the PC on the local network        (avahi-daemon)
     ── RTSP ──▶ pairing + FairPlay key exchange          (UxPlay library)
     ── TCP  ──▶ encrypted H.264 video ──▶ FFmpeg ──▶ SDL2 window
     ── UDP  ──▶ AAC-ELD audio         ──▶ FFmpeg ──▶ SDL2 audio
```

The PC announces itself as an AirPlay receiver through avahi-daemon. When you start mirroring, the iPad pairs with it and runs a FairPlay handshake to exchange the stream key. FairPlay is an undocumented Apple protocol, so this project uses the protocol library from [UxPlay](https://github.com/FDH2/UxPlay) for that part; `make` downloads it automatically. Everything after that is done here: decoding the video and audio with FFmpeg, converting colors, and showing the window and playing sound with SDL2.

## Requirements

- Linux with `avahi-daemon` running (most desktop distributions have it enabled by default)
- The PC and the iPad on the same network
- A C compiler, `make`, `git`, and the libraries listed below

## Install dependencies

**Debian / Ubuntu / Linux Mint**

```sh
sudo apt install build-essential pkg-config git \
  libavcodec-dev libavutil-dev libswscale-dev libsdl2-dev \
  libssl-dev libplist-dev libavahi-compat-libdnssd-dev avahi-daemon
```

**Fedora** (enable [RPM Fusion](https://rpmfusion.org/Configuration) first for the full FFmpeg)

```sh
sudo dnf install gcc make pkgconf-pkg-config git SDL2-devel ffmpeg-devel \
  openssl-devel libplist-devel avahi-compat-libdns_sd-devel avahi
```

**Arch Linux**

```sh
sudo pacman -S base-devel git ffmpeg sdl2 openssl libplist avahi
```

## Build

```sh
git clone https://github.com/StringLess80/glint.git
cd glint
make
```

The first build downloads UxPlay's AirPlay library into `third_party/`, so it needs an internet connection.

## Usage

1. Make sure avahi is running: `sudo systemctl enable --now avahi-daemon`
2. Start the receiver: `./glint`
3. On the iPad, open **Control Center → Screen Mirroring** and choose your PC's name.

To stop, tap **Stop Mirroring** on the iPad or close the window.

### Options

| Option | Effect |
|---|---|
| `-n NAME` | Name shown on the iPad (default: the PC's hostname) |
| `-a` | Disable audio |
| `-f` | Start in fullscreen |
| `-p PORT` | Use fixed ports `PORT` to `PORT+2` (TCP and UDP), for firewalls |
| `-v` | Print debug messages |
| `-h` | Show help |

### Keys

| Key | Action |
|---|---|
| `F`, `F11` or double-click | Toggle fullscreen |
| `Esc` | Leave fullscreen (or quit) |
| `Q` or closing the window | Quit |

The window title shows the video resolution and frame rate.

## Firewall

AirPlay ports are chosen at random on each start. If a firewall is active (Fedora enables one by default, Ubuntu does not), use `-p` to fix the ports and open them, together with UDP port 5353 for mDNS.

**ufw** (Ubuntu):

```sh
sudo ufw allow 5353/udp
sudo ufw allow 7000:7002/tcp
sudo ufw allow 7000:7002/udp
./glint -p 7000
```

**firewalld** (Fedora), until the next reboot:

```sh
sudo firewall-cmd --add-service=mdns --add-port=7000-7002/tcp --add-port=7000-7002/udp
./glint -p 7000
```

## Troubleshooting

**The PC does not appear in Screen Mirroring**
- Check that avahi is running: `systemctl status avahi-daemon`.
- The PC and the iPad must be on the same network. Guest networks and many campus networks (such as eduroam) block devices from seeing each other; use a phone hotspot instead.
- If a firewall is active, see [Firewall](#firewall).

**The video stutters or freezes**
AirPlay depends on Wi-Fi quality. Use the 5 GHz band, move closer to the router, or connect the PC to the router with an Ethernet cable.

**An app shows only its content, without its interface**
With AirPlay the iPad treats the PC as an external display, and some apps use it for a presentation mode instead of mirroring the screen. This happens with an Apple TV too.
- **GoodNotes**: while mirroring, open a document, tap **Share and Export → Mirror Entire Screen**.
- Keynote, PowerPoint and similar apps have equivalent settings.

**Colors look less vivid than on the iPad**
The iPad's display covers a wider color range (Display P3) and is very bright, while the mirrored video uses standard sRGB colors. On many laptop screens the difference is visible. Run with `-v` to print the color format the iPad declares.

**Video apps (YouTube, Netflix, …) show a black screen**
When they detect AirPlay, some video apps try to send the video in a different mode (HLS streaming) instead of mirroring, which this program does not support. DRM-protected apps may also block mirroring.

**"Lost the connection with the iPad" or "No signal from the iPad"**
The receiver restarts itself; you can reconnect right away from Control Center.

## Tests

```sh
make test
```

The tests run without an iPad and without opening real windows. They need the `ffmpeg` command-line tool (`sudo apt install ffmpeg`) to generate test streams.

- **Colors**: encodes an image with known colors in three color formats (full-range BT.709 like Apple's stream, limited-range BT.709 and BT.601), decodes it and checks the drawn pixels against the originals.
- **Receiver**: calls the same callbacks UxPlay's library uses, from a separate network thread. It checks that every video frame is decoded and displayed, that undecryptable packets are dropped, that audio is decoded and queued, that volume changes apply, and that connections are counted.

## Project layout

| Path | Contents |
|---|---|
| `src/main.c` | Command-line options |
| `src/airplay.c` | AirPlay receiver: connects UxPlay's library to the decoder, the window and audio output |
| `src/viewer.c`, `src/viewer.h` | H.264 decoder (FFmpeg), color conversion and window (SDL2) |
| `src/mirror.h` | Shared declarations |
| `tests/` | Tests and the script that generates their input files |
| `third_party/uxplay/` | UxPlay's AirPlay library, downloaded by `make` (not committed) |

## Limitations

- Screen mirroring only. Streaming a video by URL (the "AirPlay" button inside video apps) is not supported.
- Video is H.264 only, up to 1080p.
- AirPlay is a private Apple protocol; a future iPadOS version could change it.

## Credits

- [UxPlay](https://github.com/FDH2/UxPlay) and its contributors provide the AirPlay protocol implementation. It builds on [RPiPlay](https://github.com/FD-/RPiPlay) and [shairplay](https://github.com/juhovh/shairplay); the FairPlay handshake comes from EstebanKubata's [PlayFair](https://github.com/EstebanKubata/playfair). See UxPlay's README for the full list of authors.
- [FFmpeg](https://ffmpeg.org) decodes video and audio; [SDL2](https://www.libsdl.org) shows the window and plays audio.

This project is not affiliated with Apple. AirPlay, iPad and iPhone are trademarks of Apple Inc.

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The program links UxPlay's library, which contains GPLv3 code, so the built program is distributed under the GPL. `src/main.c` and `src/viewer.c` are additionally available under the MIT license, as noted in their headers.
