# What's New

Most recent changes first. Each entry: date and author in the header, then
the list of changes credited to that entry (based on beta-tester correspondence).
Entry format:

## YYYY-MM-DD - First Last
- Change description
- Another change

---

## 2026-07-19 - John Melson
- Fixed the Timeline showing a connected camera under a hardcoded, wrong
  model name ("ILCE-7RM4A") instead of the actually connected camera
- Camera-to-track assignment is now positional and deterministic: the
  daemon sorts detected cameras by GUID at startup, so the same physical
  camera always lands in the same slot regardless of USB connection order
- Track labels now show the real model + GUID of the connected camera, or
  a placeholder ("Cam1"/"Cam2"/"Cam3"/"Cam4") when that slot has no camera yet
- New camera tracks appear automatically when an additional camera is
  detected beyond the number of existing tracks (up to 4 cameras)

## 2026-07-12 - Andrzej Nowak
- Fixed bracket block duration on the timeline - length is now computed as
  the sum of the real exposure times in the series, not frame count times base SS
- Fixed a race condition when changing camera settings (ARM) - shoot/bracket/arm
  commands now reject the shot if the settings were not confirmed by the camera
- Fixed a locale bug that broke parsing of fractional EV values (0.3ev, 0.5ev, 0.7ev)
