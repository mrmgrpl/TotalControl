# TotalControl

Windows software built to control Sony Alpha series cameras — via Sony's official
Camera Remote SDK — for planning and executing the perfect solar eclipse photography
sequence, in both space and time.

**📺 Watch the intro video:** [https://youtu.be/rhCX8FMtpQg](https://youtu.be/rhCX8FMtpQg)

![TotalControl — Mission Control GUI: Sky View Simulator with timeline, contact times, and multi-camera panel](docs/images/mission_control.png)

Built for the total solar eclipse of **August 12, 2026** (Burgos/Lerma, Spain —
totality 103.9s), but usable for any total eclipse.

## What it does

TotalControl handles the two things that make or break eclipse photography:
**timing** and **composition**.

- **Local contact times, automatically.** Paste a Google Maps URL of your
  observation site and TotalControl calculates the exact local moment each
  phase of the eclipse begins and ends there.
- **Multi-camera control.** Connect up to **four cameras simultaneously**,
  each independently controlled, based on Sony's Camera Remote SDK — in
  theory, every Sony Alpha camera supported by that SDK should work.
- **A timeline you build once, calmly, weeks in advance** — not with
  shaking hands and 30 seconds left before totality. Compose every single
  shot ahead of time in one of three modes: **single frame**, **burst**, or
  **bracketing sequence**. Each block is fully adjustable (shutter speed,
  ISO, aperture) and fires exactly on the timing you set.
- **Auto-generated sequences.** Common shot patterns — a time-lapse frame
  every minute through the partial phase, or a full bracketing sequence for
  totality itself — are generated as a baseline you can then customize:
  delay blocks, delete what you don't need, insert new ones, shape it
  around your own shot list.
- **Earthshine calibration.** The long-exposure Earthshine shot (roughly
  10–15s or longer) should be fired at the *exact* midpoint of totality,
  when the corona is fully symmetric — TotalControl lets you calibrate this
  shot's timing precisely. The bracketing sequence around it is where you
  fire as many frames as your hardware allows, to catch Baily's beads and
  the diamond ring at third contact.
- **Voice-guided totality.** Load auto-generated voice track commands —
  spoken cues that walk you through what to do during totality — so you can
  actually *watch* the eclipse instead of tinkering with hardware or
  composing shots in the moment.
- **Sky View Simulator.** A virtual viewpoint matching your camera and
  hardware, including focal length, so you can preview your exact
  composition before ever standing at the site. Add multiple cameras, each
  with a different focal length, pointing angle, and tracking target — a
  long lens tracking the Sun (including solar-equator rotation angle)
  alongside a wide/fisheye camera framing the horizon, umbra approach, and
  landscape.
- **Live feed overlay for on-site calibration.** Once connected to your
  camera(s), a live feed from each camera is overlaid directly onto the
  simulated sky. Stand at your location before the eclipse, compare the
  live feed against the simulated Sun position, and immediately see whether
  framing, leveling, or tracking is off — correct it on the spot instead of
  discovering it too late during totality.
- **Corona orientation matching.** Because solar wind propagation takes
  time, the corona's current shape reflects activity from the preceding
  24 hours up to a few hundred hours back. The simulator lets you rotate
  the virtual camera to match the corona's actual currently-observed
  orientation, so your framing during totality matches reality rather than
  a geometric assumption.

## License & support

TotalControl is **open source**, released under the **GNU GPLv3** license
(see [LICENSE](LICENSE)) — completely free, no paywall, no license to buy.
Development is supported entirely through voluntary donations on Patronite.
If the project is useful to you, that's the only way to support it, and it's
entirely optional.

## Community testing — help needed before August 1, 2026

TotalControl is built on Sony's Camera Remote SDK, so in theory every Sony
Alpha camera it supports should work — but every camera body behaves
slightly differently, and the only way to catch compatibility issues and
edge cases is real people with real Alpha bodies actually running this
software before eclipse day.

**If you own a Sony Alpha camera and are even remotely considering
TotalControl for Spain or any future eclipse — please get involved now.**
Try it, break it, tell us what doesn't work.

**Hard deadline: feedback needs to arrive by August 1, 2026.** After that
date development is fully absorbed into final eclipse preparations, and no
further fixes will land before Spain. If you find something, don't sit on
it — the earlier it's reported, the more likely it gets fixed in time.

## Requirements

- Windows 10/11 x64
- CMake 4.3.3+
- MSVC (Visual Studio 2026 / VS 18)
- Sony Camera Remote SDK (CrSDK) — see `external/CrSDK/`
- A Sony Alpha camera supported by the Camera Remote SDK

## Build

```cmd
VsDevCmd.bat -arch=amd64
cmake -B out/build/x64-Release -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/x64-Release
```

See [CLAUDE.md](CLAUDE.md) for full architecture, pipe protocol, and
development details.

## Download

Pre-built releases (GUI + server + CLI + camera USB driver) are available
under [Releases](../../releases).
