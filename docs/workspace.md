# Workspace tab (desktop view)

This fork of WiVRn adds a **Workspace** tab to the menu that opens when both thumbsticks are pressed during a stream.
It shows your PC desktop live in the headset and lets you control it with the controllers, like the SteamVR desktop view.

## Features

- **Live desktop**: the PC screen is captured and streamed to the headset as a separate H.264 video, only while the tab is open.
  Nothing is encoded when the screen does not change.
- **Point and click**: aim at the desktop image and the PC mouse goes there.
  - Trigger: left click, hold to drag
  - B: right click (the *Right click* button makes the next trigger press a right click, for the left hand)
  - Thumbstick: scroll
- **Several screens**: share all your monitors once, then switch between them with the *Screen 1*, *Screen 2*… buttons, without any dialog on the PC.
- **Virtual PC keyboard** below the desktop: function keys, arrows, Home/End/PgUp/PgDn, and sticky Ctrl, Alt, Shift and Super for shortcuts (tap Ctrl then C for Ctrl+C).
  *Hide keyboard* gives the whole panel to the desktop.
- **Trackpad fallback**: when the desktop view is not available, the same area works as a trackpad that moves the mouse relatively.
- Setting **Workspace mouse speed** in *Settings → Devices*, for the trackpad.

## Requirements

- A Wayland desktop with xdg-desktop-portal supporting the ScreenCast and RemoteDesktop portals (tested on KDE Plasma 6).
- The server built with PipeWire, x264, libswscale and libsystemd (sd-bus) support.
- An Android headset for the desktop image (tested on Quest 3). The desktop Linux client only has the trackpad.
- The headset app and the server must both be built from this fork: the network protocol differs from the official WiVRn.

Mouse and keyboard go through the RemoteDesktop portal while the desktop is shown, `hid-forwarding` is not needed.
The trackpad fallback uses `hid-forwarding` (see [configuration](configuration.md#hid-forwarding)).

## First use

1. Start the server and connect the headset.
2. Open the Workspace tab: the PC shows the screen sharing dialog **on the PC monitor**.
   Select every screen you want to see, allow remote control, and confirm.
   This is remembered, later sessions start without a dialog.
3. Use *Share other screens* to show the dialog again and change the shared screens.

The choice is stored in `~/.config/wivrn/desktop_restore_token`, delete it to forget it.

## Debugging

- `WIVRN_DESKTOP_DUMP=/tmp/desktop.h264 wivrn-server` writes the desktop video to a file, playable with `mpv` or `ffplay`.
- Server messages start with `Desktop`, headset messages can be read with `adb logcat | grep -i desktop`.

## Code overview

| Part | Files |
|---|---|
| Protocol | `common/wivrn_packets.h`: `desktop_request`, `desktop_pointer`, `desktop_status`, `desktop_frame`, `stream_tab::workspace` |
| Screen capture and remote input (server) | `server/desktop/desktop_portal.*`: xdg-desktop-portal through sd-bus (GDBus does not survive the server fork) |
| Capture, conversion and encoding (server) | `server/desktop/desktop_streamer.*`: PipeWire, libswscale, x264 |
| Decoding and display (headset) | `client/scenes/desktop_view.*`, `client/shaders/desktop_yuv.glsl`: MediaCodec, Vulkan YCbCr conversion to a GUI texture |
| Tab, trackpad and keyboard (headset) | `client/scenes/stream_workspace.*`, `client/scenes/stream_gui.cpp` |
