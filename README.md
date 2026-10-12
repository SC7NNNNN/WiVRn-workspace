<h1 align="center"> WiVRn Workspace </h1>

<div align="center">
  
[![License: GPL v3](images/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0) ![CI](https://github.com/SC7NNNNN/WiVRn-workspace/workflows/Build/badge.svg) ![Format](https://github.com/SC7NNNNN/WiVRn-workspace/workflows/Format/badge.svg)
  
</div>
<p align="center"><img src="images/wivrn.svg" width="180"></p>
<h3 align="center">WiVRn with your PC desktop inside the headset</h3>

> [!IMPORTANT]
> **This is an unofficial, modified version of [WiVRn](https://github.com/WiVRn/WiVRn).**
> It is not made or supported by the WiVRn developers. Please do not report problems with this version to them,
> use the [issues of this repository](https://github.com/SC7NNNNN/WiVRn-workspace/issues) instead.

# About

WiVRn Workspace is WiVRn — an application that wirelessly streams a virtual reality game to a standalone VR
headset from a <b>Linux</b> computer — with one addition: a **Workspace** tab in the in-stream menu.

## What the Workspace tab adds

Press **both thumbsticks** during a stream to open the menu, then pick **Workspace**:

- **Your live PC desktop** in the headset, only streamed while the tab is open.
- **Point and click**: aim at the desktop and the PC mouse goes there. Trigger = left click (hold to drag),
  B = right click, thumbstick = scroll.
- **Several monitors**: share them once, then switch with the *Screen 1*, *Screen 2*… buttons.
- **Virtual PC keyboard** on a tilted panel below the menu, with function keys, arrows and sticky
  Ctrl / Alt / Shift / Super for shortcuts (tap Ctrl, then C = Ctrl+C).
- **Trackpad mode** when the desktop image is not available.

Everything else works like the official WiVRn. Details: [docs/workspace.md](docs/workspace.md).

## Supported headsets

The headset app is the same single Android app as the official WiVRn, so it should run on the same headsets.
Only the **Quest 3** has been tested with this version.

| Headset | Supported | Notes |
|:--------:|:--------:|:--------:|
| Quest 3 | ✓ | Tested |
| Quest 1, 2, 3s, Pro | ✓? | Should work, not tested |
| Pico Neo 3, Pico 4 | ✓? | Should work, not tested |
| HTC Vive Focus 3, Vive XR Elite | ✓? | Should work, not tested (laggy in official WiVRn) |
| Samsung Galaxy XR | ✓? | Should work, not tested |
| Other Android VR | ? | Cannot know |
| Play for Dream | ✖ | Not supported by WiVRn ([#465](https://github.com/WiVRn/WiVRn/issues/465)) |
| Non-Android VR | ✖ | Not Android |
| Non-VR Android | ✖ | VR required |

## Supported PCs

- **Linux only.**
- For the desktop view: a **Wayland** desktop with screen sharing and remote control portals.
  Tested on **KDE Plasma 6**; GNOME should work too. On other desktops (Hyprland, Sway…) the picture may work
  but mouse and keyboard may need `hid-forwarding` (see [docs/workspace.md](docs/workspace.md)).
- Install steps for **Arch**, **Fedora**, **Debian / Ubuntu** and **openSUSE** are in [Getting started](#pc).

> [!WARNING]
> The PC and the headset must **both** use WiVRn Workspace, from the same release.
> The official WiVRn app (Meta Store, official APK) cannot connect to this PC version, and this headset app
> cannot connect to the official PC version.

# Getting started

## PC

There is no ready-made package or Flatpak of this version: it is built on your computer, which takes a few minutes.
If you have the **official** WiVRn installed (package or Flatpak), uninstall it first.

Pick your distribution below. Click its name to open the steps.

<details><summary><b>Arch Linux</b>, CachyOS, EndeavourOS, Manjaro and other Arch-based</summary>

```bash
git clone https://github.com/SC7NNNNN/WiVRn-workspace.git
cd WiVRn-workspace/packaging/arch
makepkg -si
```

This installs the package `wivrn-workspace` (server + dashboard), which replaces the official `wivrn-server` and
`wivrn-dashboard` packages. To update later, run `git pull` in the same folder and `makepkg -si` again.
To uninstall: `sudo pacman -R wivrn-workspace`.
</details>

<details><summary><b>Fedora</b> 43 or newer</summary>

The x264 video encoder comes from [RPM Fusion](https://rpmfusion.org/Configuration). Enable it if you have not already:

```bash
sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
```

Install the build tools and libraries:

```bash
sudo dnf install git cmake ninja-build gcc-c++ glslang glslang-devel glslc spirv-tools python3 gettext-devel \
  avahi-devel libavcodec-free-devel libavfilter-free-devel libavutil-free-devel libswscale-free-devel x264-devel \
  libbsd-devel boost-devel libcap-devel cjson-devel cli11-devel eigen3-devel libnotify-devel pipewire-devel \
  systemd-devel libva-devel vulkan-loader-devel vulkan-headers wayland-devel libXrandr-devel json-devel \
  openssl-devel libarchive-devel librsvg2-devel librsvg2-tools libpng-devel libdrm-devel \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qttools-devel extra-cmake-modules \
  kf6-kirigami-devel kf6-ki18n-devel kf6-kcoreaddons-devel kf6-qqc2-desktop-style kf6-kiconthemes-devel \
  kf6-kirigami-addons-devel qcoro-qt6-devel
```

Then follow [Build and install](#build-and-install) below.
</details>

<details><summary><b>Debian</b> 13 or newer, <b>Ubuntu</b> 26.04 or newer, and distributions based on them</summary>

Ubuntu 24.04 and distributions based on it (Linux Mint 22, Pop!_OS 24.04, Zorin OS 18…) are **too old**:
their Vulkan libraries are older than what WiVRn needs.

```bash
sudo apt install git cmake ninja-build pkg-config build-essential bsdextrautils glslang-tools spirv-tools python3 gettext \
  libavahi-client-dev libavahi-glib-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libx264-dev \
  libbsd-dev libboost-all-dev libcap-dev libcjson-dev libcli11-dev libeigen3-dev libgl-dev libglu1-mesa-dev \
  libnotify-dev libpipewire-0.3-dev libsystemd-dev libudev-dev libva-dev libvulkan-dev libwayland-dev \
  libx11-xcb-dev libxcb-randr0-dev libxrandr-dev nlohmann-json3-dev libssl-dev libarchive-dev librsvg2-dev \
  librsvg2-bin libpng-dev libdrm-dev qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
  qt6-declarative-private-dev qt6-tools-dev extra-cmake-modules libkirigami-dev libkf6i18n-dev \
  libkf6coreaddons-dev libkf6qqc2desktopstyle-dev libkf6iconthemes-dev qcoro-qt6-dev \
  qml6-module-org-kde-kirigamiaddons-formcard
```

Then follow [Build and install](#build-and-install) below.
</details>

<details><summary><b>openSUSE Tumbleweed</b></summary>

The x264 video encoder comes from the [Packman](https://en.opensuse.org/Additional_package_repositories#Packman) repository. Add it if you have not already:

```bash
sudo zypper ar -cfp 90 https://ftp.gwdg.de/pub/linux/misc/packman/suse/openSUSE_Tumbleweed/ packman
sudo zypper refresh
```

Install the build tools and libraries:

```bash
sudo zypper install git cmake ninja gcc-c++ glslang-devel spirv-tools python3 gettext-tools boost-devel \
  rsvg-convert extra-cmake-modules kirigami-addons6 libx264-devel \
  'pkgconfig(avahi-client)' 'pkgconfig(avahi-glib)' 'pkgconfig(libavcodec)' 'pkgconfig(libavfilter)' \
  'pkgconfig(libavutil)' 'pkgconfig(libswscale)' 'pkgconfig(libbsd)' 'pkgconfig(libcap)' 'pkgconfig(libcjson)' \
  'cmake(CLI11)' 'pkgconfig(eigen3)' 'pkgconfig(libnotify)' 'pkgconfig(libpipewire-0.3)' 'pkgconfig(libsystemd)' \
  'pkgconfig(libudev)' 'pkgconfig(libva)' 'pkgconfig(vulkan)' 'pkgconfig(wayland-client)' 'pkgconfig(xrandr)' \
  'pkgconfig(nlohmann_json)' 'pkgconfig(openssl)' 'pkgconfig(libarchive)' 'pkgconfig(librsvg-2.0)' \
  'pkgconfig(libpng)' 'pkgconfig(libdrm)' 'cmake(Qt6Core)' 'cmake(Qt6Quick)' 'cmake(Qt6QuickControls2)' \
  'cmake(Qt6Widgets)' 'cmake(Qt6Network)' 'cmake(Qt6Test)' 'cmake(Qt6LinguistTools)' 'cmake(Qt6QmlPrivate)' \
  'cmake(KF6Kirigami)' 'cmake(KF6I18n)' 'cmake(KF6CoreAddons)' 'cmake(KF6QQC2DesktopStyle)' \
  'cmake(KF6IconThemes)' 'cmake(QCoro6Core)' 'cmake(QCoro6Network)' 'cmake(QCoro6Qml)'
```

Then follow [Build and install](#build-and-install) below.
</details>

<details><summary><b>Other distributions</b> (NixOS, Gentoo, Void…)</summary>

Install the equivalent of the packages listed above for your distribution (Vulkan 1.4.304 or newer, CMake 3.28
or newer, Qt 6 and KDE Frameworks 6 for the dashboard), then follow [Build and install](#build-and-install) below.
See also [building](docs/building.md).

Immutable distributions (Bazzite, SteamOS, Fedora Silverblue…) cannot install the build tools this way.
</details>

### Build and install

For every distribution except the Arch-based ones (they use `makepkg` above):

```bash
git clone https://github.com/SC7NNNNN/WiVRn-workspace.git
cd WiVRn-workspace
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
  -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_DASHBOARD=ON
cmake --build build
sudo cmake --install build
echo 'PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1' | sudo tee /usr/lib/environment.d/wivrn.conf
```

The last line lets Steam games find WiVRn; log out and back in once after installing.
The first `cmake` step downloads some parts of WiVRn from the internet, if it fails just run it again.

- **Update**: in the `WiVRn-workspace` folder, run `git pull`, then the `cmake --build build` and
  `sudo cmake --install build` lines again.
- **Uninstall**: in the `WiVRn-workspace` folder, run `sudo xargs rm < build/install_manifest.txt` and
  `sudo rm /usr/lib/environment.d/wivrn.conf`.

> [!NOTE]
> Only the Arch Linux steps have been tested by the author. The package lists for the other distributions were
> checked against each distribution's package database, but not test-built. If something is missing,
> please [open an issue](https://github.com/SC7NNNNN/WiVRn-workspace/issues).

### Steam and OpenVR games

For OpenVR and Steam compatibility, you also need a compatibility library such as [xrizer](https://github.com/Supreeeme/xrizer/) or [OpenComposite](https://gitlab.com/znixian/OpenOVR/).

## Headset

1. Enable [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/#enable-developer-mode) on the headset (Quest) and install `adb` on the PC.
2. Download `WiVRn-workspace-quest.apk` from the [latest release](https://github.com/SC7NNNNN/WiVRn-workspace/releases/latest).
3. Connect the headset with a USB cable and run:
   ```bash
   adb install -r WiVRn-workspace-quest.apk
   ```
4. The app appears in *Library → Unknown Sources*. It installs next to the official WiVRn app.

> [!NOTE]
> The dashboard's first-start wizard links to the Meta Store: that is the **official** app, which cannot
> connect to this version. Use the APK from the releases instead.

> [!TIP]
> If the headset fails to connect to the computer, see [troubleshooting](#troubleshooting).

# Usage

## Running

### Prerequisites
Avahi must be running:
```bash
systemctl enable --now avahi-daemon
```

- If a firewall is installed, open port 5353/UDP for Avahi and ports 9757/UDP+TCP for WiVRn.
- For example, if using UFW run `ufw allow 5353/udp` and `ufw allow 9757`.

### Start the PC server process
The graphical frontend is listed as "WiVRn server" in the application list, and is `wivrn-dashboard` on command line. On first start, a wizard will guide you through the initial steps.

The actual server for headless usage is `wivrn-server`. A systemd user service named `wivrn` is also installed, which can be enabled to automatically start on login with `systemctl --user enable --now wivrn`.

For Steam games, you may need to set launch options in the Steam properties for each game you want to run. Either the dashboard or command line output will display the launch command if it is required.

### Start the headset application
On the headset, start `WiVRn` from *Library → Unknown Sources*.

On first start, it will ask if you want to enable some features such as microphone, hand tracking, eye tracking, etc., as they will require permissions to be granted. It is possible to grant those later from the Settings tab.

It is highly recommended to use default settings and only tweak them if you experience issues.

### Connect to the server
The headset application will start on a server list. Your computer should be visible and have a connect button. Simply click it to start streaming. The first time, the headset asks for a PIN shown on the PC (pairing).

When the headset is connected, wivrn-server sets the OpenXR and OpenVR configuration to use WiVRn. Thus, applications will only be able to run in VR once the headset connection is established. The configuration is reverted once the connection ends and all running VR applications are closed. 

The headset connection also triggers the creation of a virtual speaker and, if enabled in the headset app settings, a microphone. You will have to set them as the default output and input devices in your system audio configuration. This setting persists to future sessions until you change the defaults to other devices.

### Start an application
When the headset is connected and no XR application is running, it will show an application launcher. Applications in that list are sourced from:
- Steam games that are flagged as VR. Steam may need to be restarted for the list to be updated when new games are installed.
- .desktop files that contain `X-WiVRn-VR` in the `Categories` section. Files are searched in [standard locations](https://specifications.freedesktop.org/desktop-entry/latest/file-naming.html#desktop-file-id) which usually include `~/.local/share/applications` and `/usr/share/applications/`.

You can set an application to be started automatically when your headset is connected, in the dashboard settings or [manually](docs/configuration.md#application).

### Use the Workspace tab
1. During a stream, press both thumbsticks and open **Workspace**.
2. The first time, the screen sharing dialog appears **on the PC monitor** (not in the headset): select every
   screen you want to see, allow remote control, and confirm. This is remembered for next time.
3. *Share other screens* shows the dialog again if you want to change the shared screens.

# Building

See [building](docs/building.md) for building the [dashboard](docs/building.md#dashboard), [server (PC)](docs/building.md#server-pc), and [client (headset)](docs/building.md#client-headset).
The desktop view needs the server to be built with PipeWire, x264, libswscale and libsystemd.


# Configuration
Most settings are controlled through the headset app, while the server has configuration for items that are specific to the server. Use the dashboard to edit the latter, or see [configuration](docs/configuration.md) for editing it manually.

# Troubleshooting
<details><summary>My computer is not seen by the headset</summary>

If the server list is empty in the headset app:
- Make sure your computer is connected on the same network as your headset
- Check that avahi is running with `systemctl status avahi-daemon`, if it is not, enable it with `systemctl enable --now avahi-daemon`
- If you have a firewall, check that port 5353 (UDP) is open</details>

<details><summary>My headset does not connect to my computer</summary>
  
- If you have a firewall, check that port 9757 (UDP and TCP) is open
- The PC and the headset must both use WiVRn Workspace from the same release. The official WiVRn app cannot connect.</details>

<details><summary>The Workspace tab says it is waiting for the screen sharing dialog</summary>

- The dialog is shown on the **PC monitor**, take the headset off and confirm it there.
- If you refused it, press *Start desktop view* to ask again.
- To forget the saved screen choice, delete `~/.config/wivrn/desktop_restore_token`.</details>

<details><summary>How do I use a wired connection manually?</summary>

- Make sure the WiVRn Server is installed and running on your computer
- Make sure the WiVRn Workspace app is installed on your headset
- After starting the "WiVRn Server" on your computer and ensuring your device is connected to your PC via cable, run the following in your terminal (Note: using `adb` on some devices may require developer mode to be enabled):
   - ```bash
      adb reverse tcp:9757 tcp:9757
      adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost" org.meumeu.wivrn.local
      ```
- You can now continue the pairing process as documented in the running section.</details>

<details><summary>How do I see server logs when using the dashboard?</summary>

- Click **Troubleshoot > Open server logs**
- Or, navigate to `${XDG_STATE_HOME}/wivrn/wivrn-dashboard`
  - In other words, on common setups, the dashboard writes logs to `~/.local/state/wivrn/wivrn-dashboard`</details>

<details><summary>My NVIDIA GPU P-State is limited to P2 instead of reaching the highest P0 while using the NVIDIA NVENC encoder</summary>
    
- See [nvenc](docs/nvenc.md) for troubleshooting.</details>

<details><summary>I have high motion latency, black borders following my view, hear corrupted audio or see a corrupted, pixelated image</summary>

- When connecting through USB, make sure the headset isn't connected through WiFi (switch off WiFi)
- Reset the settings using the button at the bottom of the settings tab
- Try switching to software encoding
- Decrease the bitrate
- Decrease the resolution in the WiVRn app
- Connect through USB or use a better WiFi router.

Note: WiVRn isn't properly optimized for NVIDIA GPUs due to the lack of developers with NVIDIA hardware. Motion latency may be significantly worse at rendering resolutions higher than default.</details>

# Support

For problems with **this version** (including the Workspace tab), open an issue here:
https://github.com/SC7NNNNN/WiVRn-workspace/issues

The official WiVRn community ([Discord](https://discord.gg/EHAYe3tTYa), [Matrix](https://matrix.to/#/#linux-vr-adventures:matrix.org)) supports the official WiVRn only.
Please do not ask them about this modified version.


# Contributing

## Translations

See [translating](docs/translating.md) for procedure.


# Credits
WiVRn Workspace is based on [WiVRn](https://github.com/WiVRn/WiVRn), made by the WiVRn developers. All the credit for WiVRn itself goes to them.

WiVRn uses the following software:
- [ambientCG](https://ambientcg.com/)
- [Avahi](https://www.avahi.org/)
- [Boost.Locale](https://github.com/boostorg/locale)
- [Boost.PFR](https://github.com/boostorg/pfr)
- [CLI11](https://github.com/CLIUtils/CLI11)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [fastgltf](https://github.com/spnda/fastgltf)
- [ffmpeg](https://ffmpeg.org/) optional, for hardware encoding on AMD/Intel
- [FreeType](https://freetype.org/)
- [glm](http://glm.g-truc.net/)
- [HarfBuzz](https://harfbuzz.github.io/)
- [librsvg](https://wiki.gnome.org/Projects/LibRsvg)
- [Monado](https://monado.freedesktop.org/)
- [nvenc](https://developer.nvidia.com/nvidia-video-codec-sdk) optional, for hardware encoding on NVIDIA
- [qCoro](https://qcoro.dev/)
- [Qt 6](https://www.qt.io/) optional, for the dashboard
- [spdlog](https://github.com/gabime/spdlog)
- [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
- [WebXR input profiles](https://www.npmjs.com/package/@webxr-input-profiles/motion-controllers)
- [x264](https://www.videolan.org/developers/x264.html) optional, for software encoding
