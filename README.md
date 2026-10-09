# TrackIR for macOS

Use a **NaturalPoint TrackIR 5** head tracker on a Mac, natively. NaturalPoint only ships TrackIR software for
Windows; this project is an independent macOS reimplementation of it, written from a reverse-engineering study of
TrackIR 5.5.3.

- **Menu-bar app**: start/stop, pause and recenter (global hotkeys), live view of what the camera sees, a curve
  editor for each axis, TrackIR profile support, open at login.
- **X-Plane 11/12 plugin**: your head moves the pilot's view in the 3-D cockpit. No opentrack, no network setup.
- **opentrack output** (UDP), for any other app that opentrack supports.
- **Windows games under Wine/CrossOver**: drop-in `NPClient.dll` / `NPClient64.dll` so games see normal TrackIR.
- No kernel extension and no third-party libraries: it talks to the camera directly over USB.

> Unofficial project, not affiliated with or endorsed by NaturalPoint. TrackIR is a trademark of NaturalPoint, Inc.
> No NaturalPoint code or data is included here. Use it with your own TrackIR hardware and software.
> Rights holders with concerns: please see [Legal](#legal).

## What you need

- A Mac with macOS 12 or later (Apple Silicon or Intel).
- A **TrackIR 5** camera and a **TrackClip** (reflective) or **TrackClip PRO** (LED). An ordinary webcam will not
  work, because the TrackIR camera finds the markers in hardware.
- The Xcode Command Line Tools (`xcode-select --install`). That is all the project needs to build.
- Optional: NaturalPoint's free Windows installer `TrackIR_5.5.3.exe` from naturalpoint.com, for TrackIR's own
  profiles (recommended) and for older TrackIR 5 revisions. See step 2.

## Status

| | |
|---|---|
| TrackIR 5, current model (USB id 131d:0159) | Works: tested on a real camera (123 frames/s) |
| Menu-bar app, axis check, opentrack output | Works |
| X-Plane plugin | Built and tested against a simulated X-Plane; not yet flown in the real sim. Reports welcome |
| Older TrackIR 5 (131d:0157 / 0158) | Implemented, untested |
| Wine/CrossOver `NPClient.dll` | Verified against NaturalPoint's own DLL in an emulator; not yet tried with a real game |
| TrackIR 4 and older | Not supported |

## Install

### 1. Build

```sh
git clone https://github.com/dmucli/trackir-macos.git
cd trackir-macos
make app        # the menu-bar app: build/TrackIR-macOS.app
make xplane     # the X-Plane plugin (downloads the X-Plane SDK into build/ the first time)
```

Move `build/TrackIR-macOS.app` to your Applications folder. The app is built and signed on your own Mac, so it
opens without Gatekeeper warnings.

### 2. Import TrackIR's profiles (recommended)

TrackIR's camera data and its stock profiles (Default, Flying, Smooth, …) belong to NaturalPoint, so they are not
included. Copy them from the official installer instead. Nothing from the installer is ever run.

```sh
brew install sevenzip msitools
tools/extract-trackir.sh ~/Downloads/TrackIR_5.5.3.exe ~/TrackIR5
build/trackir-mac extract-fpga ~/TrackIR5/TrackIR5.exe
```

This puts the profiles and camera data in `~/Library/Application Support/TrackIR-macOS/`. The current TrackIR 5
also works without this step, with 1:1 curves and without the camera's authenticity check.

### 3. Plug in the camera and start the app

Plug in the TrackIR (a USB hub is fine), put the clip on and open **TrackIR for macOS**. A target icon appears in
the menu bar, and the camera's lights come on. Choose **Live View** to see the markers and the six axes move.

### 4. Check your axes (once)

Choose **Run Axis Check in Terminal…** from the menu and follow the prompts. Have a tape measure ready. The check
takes about two minutes:

- it measures jitter while you hold still;
- it asks for the real camera-to-clip distance, to correct the lens scale;
- it asks you to turn, look up, tilt, slide, rise and lean, and fixes any axis that moves the wrong way.

The results are saved, and the app picks them up. Switch **Tracking** back on in the menu afterwards.

## Using the menu-bar app

| Menu item | What it does |
|---|---|
| Tracking | Turns the camera on or off |
| Pause (F9) | Freezes the view where it is; press again to resume |
| Recenter (F12) | Makes your current head position the centre |
| Live View… | The camera image (markers) and the six output axes |
| Profile | Choose a profile (Default, Flying, …), import a `.xml`, or edit the curves |
| Settings… | Clip type, IR lights, smoothing, marker threshold, curves, opentrack output, hotkeys |
| Run Axis Check in Terminal… | The guided check from step 4 |
| Open at Login | Starts the app when you log in |

**Hotkeys** work in every app, including full-screen games. On a Mac keyboard, press `fn` with the F-key unless
*Use F1, F2, etc. keys as standard function keys* is on in System Settings › Keyboard. You can change both keys in
Settings › Output & Keys.

**Curves** (Settings › Curves): each axis has a curve of *speed* against head angle. The point at the centre sets
the dead zone, and the higher a point, the faster the view turns at that head angle. The red line shows your
current head position and the resulting view angle. Changes apply immediately. **Save Profile As…** writes a
TrackIR-compatible `.xml`.

**Clip type**: with a TrackClip PRO, choose it in Settings › Camera and switch the camera's IR lights off.

**If the view shakes while you hold still**, raise *Smoothing* in Settings › Camera (try 0.5). The filter adapts:
it smooths heavily when you are still and lightly when you move, so quick glances stay responsive.

**If tracking is jumpy**, open the Live View. It should show exactly three green dots. Extra grey dots are
reflections or other infrared sources (sunlight, glasses, shiny surfaces). Remove them, or raise the marker
threshold in Settings › Camera.

## X-Plane

1. Copy the folder `build/xplane/TrackIR-macOS` into `X-Plane 12/Resources/plugins/` (X-Plane 11 works the same).
2. Start the menu-bar app, then X-Plane. In the 3-D cockpit view, the pilot's head follows yours.
3. The plugin's menu is under **Plugins › TrackIR (macOS)**: *Head tracking* (on/off), *Pause*, *Recenter* and a
   status line.

Notes:

- The same actions are X-Plane commands, `trackir_macos/toggle`, `trackir_macos/pause` and
  `trackir_macos/recenter`. Bind them to keys or joystick buttons in X-Plane's settings.
- Tracking starts from wherever X-Plane's head is when it takes over. While it is on, it controls the view, so
  keyboard and mouse view moves are overridden. To adjust your seat position, switch *Head tracking* off, move the
  view, then switch it back on.
- The plugin does nothing in outside views and in VR.
- How far the view turns for a given head movement is set by the profile. TrackIR's *Default* profile turns the
  view about 6× your head angle, so you can look behind you while still seeing the screen.

## Other outputs

**opentrack**: in Settings › Output & Keys, tick *Send opentrack “UDP over network” data* (default
127.0.0.1:4242). In opentrack, choose the input *UDP over network*.

**Windows games under Wine/CrossOver**:

1. `make npclient` (needs `brew install mingw-w64`).
2. Copy `build/wine/NPClient.dll` and `NPClient64.dll` into the bottle, e.g. `C:\TrackIR\`.
3. In the bottle's registry, set `HKEY_CURRENT_USER\Software\NaturalPoint\NATURALPOINT\NPClient Location` to
   `C:\TrackIR\` (with the trailing backslash).
4. Keep the menu-bar app running. Games find it through `Z:\tmp\TrackIR-macOS.bridge`.

## Command line

The app bundles `trackir-mac`, which can also be used on its own (from `build/`):

```sh
trackir-mac list                    # is the camera detected?
trackir-mac run --verbose           # track in the terminal (Ctrl-C to stop)
trackir-mac run --profile ~/Library/Application\ Support/TrackIR-macOS/Profiles/flying.xml --udp 127.0.0.1:4242
trackir-mac check                   # the guided axis check
trackir-mac recenter                # recenter a running tracker
trackir-mac --help                  # all options
```

Settings are shared with the app, in `~/Library/Application Support/TrackIR-macOS/settings.ini`. Command-line options
override them for one run. Only one program can use the camera at a time, so quit the app (or switch Tracking off)
before running `trackir-mac run`.

## Troubleshooting

| Problem | What to try |
|---|---|
| `no NaturalPoint (131d) devices found` | Check System Information › USB lists a device with vendor id 0x131d. Try another port or hub |
| The camera's lights stay off | Normal until the app or `trackir-mac run` starts it |
| Lots of markers / pose jumps around | The camera faces something bright or reflective, or is lying face down. Check the Live View |
| Clip not found at the edges of your movement | Lower the marker threshold, or move the camera so you stay inside its view |
| An axis moves the wrong way | Run the axis check. To flip an axis only for one profile, use *Inverted* in Settings › Curves |
| X-Plane plugin status says "tracker not running" | Start the menu-bar app and make sure Tracking is ticked |

## Building and testing

```sh
make              # build/trackir-mac
make app          # build/TrackIR-macOS.app (includes trackir-mac)
make xplane       # build/xplane/TrackIR-macOS/mac_x64/TrackIR-macOS.xpl (Intel + Apple Silicon)
make test         # unit and integration tests, with camera emulators
make xplane-test  # loads the plugin into a fake X-Plane and drives it
make npclient     # Wine DLLs (brew install mingw-w64)
make verify VENDOR=~/TrackIR5   # byte-exact comparison with your own copy of the vendor binaries under an
                                # x86 emulator (python3 -m venv .venv && .venv/bin/pip install pefile unicorn)
```

```
src/protocol/   camera USB protocols (TrackIR 5 "secure" protocol, older FPGA-based cameras), frame decoding
src/vision/     marker blobs, lens model, TrackIR's pose solver, recentring
src/output/     profile curves, NPClient bridge, opentrack UDP
src/usb/        USB through Apple's IOUSBHost framework
src/app/        tracking engine, settings, command-line tool
src/macapp/     menu-bar app (Cocoa)
xplane/         X-Plane plugin
wine/           NPClient.dll replacement for Wine/CrossOver
tools/          installer extraction, emulator-based verification against the vendor binaries
tests/          unit tests, camera emulators, fake XPLM
docs/           PROTOCOL.md: the reverse-engineered protocol and algorithms
```

## How it works

```
TrackIR camera ──USB──▶ trackir-mac engine ──▶ /tmp/TrackIR-macOS.bridge ──▶ X-Plane plugin
 (finds the IR markers)   (pose + profile)                          └──────▶ NPClient.dll (Wine games)
                                           └──▶ UDP ──▶ opentrack
```

The camera reports the bright infrared spots it sees, not video. The engine finds the clip's three markers, solves
the head pose with the same method TrackIR uses, applies the profile curves, and publishes the result.
[docs/PROTOCOL.md](docs/PROTOCOL.md) documents everything that was recovered: the camera handshake and command
protocol, the frame formats, the lens and pose model, and the game-side shared-memory interface.

## Legal

- This is an independent implementation for interoperability with TrackIR hardware the user owns. It contains no
  NaturalPoint code, firmware, keys or profiles. The tools above copy what is needed from the user's own copy of
  NaturalPoint's installer, on the user's own machine.
- The X-Plane SDK is downloaded at build time and is under its own licence (Laminar Research / Sandy Barbour and
  Ben Supnik).
- Released under the [MIT licence](LICENSE).
- **Rights holders:** if you represent NaturalPoint or another rights holder and have a concern about anything in
  this repository, please [open an issue](https://github.com/dmucli/trackir-macos/issues) and it will be looked at
  promptly, including removing material where appropriate.
