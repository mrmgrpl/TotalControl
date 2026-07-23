# What's New

---

## 2026-07-23

### Andrzej Nowak
- Fixed multi-camera control: with more than one camera connected, only one camera would ever fire at a time and all timing would drift badly. Each camera now gets its own dedicated connection and scheduling, instead of all cameras sharing one at a time - confirmed on hardware with 4 cameras firing brackets within milliseconds of each other instead of queueing behind one another
- Fixed the GUI freezing (needing the process killed) when stopping a running sequence, sometimes even between two commands - a stuck request now times out and cancels cleanly after 10 seconds instead of blocking forever
- Fixed the camera server occasionally failing to accept new camera connections when several connected at nearly the same time
- The camera status panel and CLI now show a readable drive-mode name (e.g. "Bracket 1EV/9/Cont") instead of a raw hex code
- Log timestamps in the server, GUI, and CLI are now consistently full ISO 8601 with a stable four-digit fraction of a second
- Added a "Bracket ARM Calibration" preset to measure, per camera model, how long each one actually takes to apply a settings change between shots - confirmed on hardware that this varies a lot by camera (some models 8-10x slower than others), and the Timeline now schedules each camera's gap using its own measured number instead of one guess for every model
- Added a "Bracket SS Sweep" preset to validate that the Timeline's predicted bracket duration holds up across the full range of shutter speeds, not just the one speed it was originally calibrated against
- Fixed a bracket duration prediction bug that could estimate a wide bracket (5 or 9 shots) at a slow base shutter speed as taking far longer than physically possible - one case predicted ~200 seconds for a block that actually takes about 8. The formula was assuming shutter speeds beyond what any camera (or the SDK) can produce - real shutter speeds top out at 30 seconds. Each shot in a bracket is now checked against the camera's real, standard shutter-speed steps before its time gets added up, so predictions can no longer exceed what's physically possible
- Restructured the per-camera bracket-timing calibration data so it's keyed only by camera model and shot count, not also by EV step - measurements had shown the EV step was never actually affecting the per-shot overhead, and keeping it as a key was the direct cause of a data-corruption bug (a shutter-speed sweep test could silently overwrite good calibration numbers with an average of samples taken at wildly different, unrelated shutter speeds). Factory-default calibration for all four tracked camera models now ships as real measured data in a bundled database file instead of being hardcoded in the app
- Sped up connecting to multiple cameras by removing a redundant USB re-scan that was happening once per camera on top of the one already done to find them in the first place - measured on hardware with 4 cameras: connect time dropped from about 27 seconds per camera (108s total) to well under a second per camera (under 3s total)
- Cleaned up the camera server's startup banner: version number was stale (hadn't been updated since May), and it printed internal details (pipe name, CLI stop command) that aren't useful to an end user

### Alessandro Pessi
- Eclipse contact-time predictions (C1/C2/Max/C3/C4) now use a Delta-T value (the correction between clock time and Earth's actual rotation) fetched from the IERS's own published bulletin and refreshed automatically once a day, instead of a fixed value that was already several seconds stale for this eclipse - matches the ~6 second discrepancy Alessandro found against besselianelements.com. Falls back to the last successfully fetched value if today's update fails, so a network hiccup never leaves the app without one. The current value is now also shown in the Solar Simulator status bar

### John Melson
- Confirmed on 2+ physical cameras that camera-to-track assignment works correctly: each camera lands in the same slot every time regardless of USB connection order, tracks show the right model+GUID for each connected camera, and a new track appears automatically when an additional camera is detected

## 2026-07-20

### Maciej Szupiluk
- Inverted when the Moon renders as a solid black disc vs. its photo texture in the Solar Simulator (Maciej Szupiluk's idea): now black during the partial phases (C1-C2 and C3-C4) and shows the photo during totality (C2-C3) - previously the other way around. Falls back to the old geometric heuristic (moon fully covering the sun) when contact times haven't been calculated yet
- "Connect camera" / "Test picture" / "Disconnect" now split the available width evenly (was a fixed 135px each, leaving dead space on wider panels)
- Latitude and Longitude now sit on one line instead of two, saving vertical space in the LOCATION section
- Fixed "TotalControlCLI quit" not actually closing the camera server process while another client (e.g. the GUI) was still connected - the server now fully stops as soon as any client sends quit, regardless of how many other clients are connected
- Fixed the GUI silently telling the camera server to fully shut down every time the GUI closed, even from a plain restart - not just the explicit "Disconnect" button. This defeated the whole point of the confirmation dialog below (its own text promised the server keeps running) and lost the camera connection unnecessarily; now closing the GUI only disconnects the GUI, leaving the server and cameras running
- Added on-hover tooltips to nearly every field in the GUI (ECLIPSE, LOCATION, TIME, SERVER/CAMERA STATUS, BLOCK INSPECTOR, EXECUTE, Solar Simulator status bar, SUVI ALIGNMENT, LIVE VIEW, ACTION LIBRARY, CAMERA CONFIG, OPTIONS), explaining what each value means and where it comes from
- Fixed the Options window: the "besselianelements.com" domain was repeated three times in one small window - now stated once; widened the window so no line clips and no scrollbar is needed; the status line at the bottom incorrectly said "BE REST API active" when a key was set (BE is actually the local *fallback* used when there's no key) - now correctly says "IQP REST API active"
- Fixed bracket block duration on the timeline - length is now computed as the sum of the real exposure times in the series, not frame count times base SS
- Fixed a race condition when changing camera settings (ARM) - shoot/bracket/arm commands now reject the shot if the settings were not confirmed by the camera
- Fixed a locale bug that broke parsing of fractional EV values (0.3ev, 0.5ev, 0.7ev)
- Expanded the IQP/BE model tooltips with proper attribution: IQP names its source (besselianelements.com); BE now spells out its full name ("Besselian Elements"), and credits Fred Espenak (NASA GSFC, elements dataset), Jean Meeus ("Elements of Solar Eclipses", 1989, algorithm), and Greg Miller (celestialprogramming.com, the public-domain reference code this app's implementation follows)
- Moved the "IQP / BE" primary-model selector to sit next to the Altitude field in the LOCATION section, instead of its own row above the contact table; label reworded ("Primary:" -> "Choose prediction model") and moved after the two options
- Relabeled the GE contact-times table headers to "GE C1/C2/MAX/C3/C4" and removed the redundant "GE contact times (UTC)" caption above it
- Added an explicit "Primary: IQP / BE" choice above the contact-time table (TIME section), so it's clear which of the two independent calculation engines actually drives generated Timeline blocks (One Picture Per Minute, Totality Brackets, audio presets, Snap to Seconds), the T- countdown, and the Loc (local time zone) column, instead of silently always preferring IQP. Falls back to the other engine automatically if the chosen one isn't available yet; the IQP and BE columns keep showing their own results side by side regardless of the choice
- Fixed the GOES-19 SUVI corona image's burned-in date/time stamp and NOAA logo showing up overlapping the solar simulator - the bottom edge of the image is now masked out transparently before it's displayed
- The IQP/BE/Loc/T- column headers and the C1/C2/Max/C3/C4/Rise/Set row labels in the contact-time table now have their own tooltips, addressing the "column names aren't explained" report
- Moved the GE (greatest eclipse) contact times out of that table into their own small table in the ECLIPSE section, next to the eclipse picker - GE times are a property of the eclipse itself, not of the observer's location, so they didn't belong alongside the location-dependent IQP/BE/Loc columns

### Alexandru Barbovschi
- Added a note to the BE tooltip that it uses a smooth, circular lunar limb with no libration/limb-profile correction, so C2/C3 can differ by roughly 1-2 seconds from limb-corrected tools (C1/C4/Max are far less sensitive to this and should stay in close agreement); closing line now reads "Use this model if you can't obtain an IQP API key."
- Added a confirmation dialog before closing the GUI (X button, Alt+F4, or File > Quit), stating that a restart takes about 5 seconds, and warning that the camera server keeps running if it's still connected
- Added the same protection to the camera server (TotalControlSRV): closing its console window or pressing Ctrl+C now asks for confirmation first, warning that reconnecting to the cameras afterward can take up to 60 seconds - system logoff/shutdown are not blocked, only operator actions

### John Melson
- Added remote control of the camera's Focus Magnifier (Live View zoom) -- needed for passive/manual optics like a telescope, which never send the focus-ring-turning signal a native Sony lens uses to trigger this automatically. Four buttons (Off / x1.0 / x5.9 / x11.9) next to each camera's Live View opacity slider in SIMULATOR CONFIG; confirmed live on real hardware -- pressing a button visibly changes the zoom shown on the camera's own screen, not just the value TotalControl reads back
- Added a `dump_props` diagnostic command (pipe + CLI-testable) that snapshots every property code and value the camera currently reports -- used to find the Focus Magnifier property's real, undocumented wire format (a compound 40-bit value, not the simple small-integer enum its SDK header comments would suggest) by diffing a before/after snapshot while manually cycling the setting on the camera; kept as a general-purpose tool for investigating other undocumented properties in the future
- Added a "Camera Setup" how-to window (About menu > Camera Setup: Manual mode, RAW, Manual Focus, USB Connection Mode, connect the USB cable), plus a matching tooltip on the "Connect camera" button, so it's no longer necessary to already know these settings by memory before the first connection. The USB Connection Mode step now also lists the exact menu label per camera series ("Remote Shooting" / "Remote Shoot (PC Remote)" / "Remote Shoot/Trn.", sourced from Sony's official Imaging Edge docs, linked in the window) - confirms this setting really is model-dependent
- Fixed TotalControlSRV.log being wiped every time the server restarts - beta testers routinely restart it several times while troubleshooting a connection, which was destroying the exact evidence needed to diagnose the problem. The log now appends across restarts with a "=== SRV start ===" separator marking each session, matching how TotalControlGUI.log already works
- Fixed the Timeline showing a connected camera under a hardcoded, wrong model name ("ILCE-7RM4A") instead of the actually connected camera
- Camera-to-track assignment is now positional and deterministic: the daemon sorts detected cameras by GUID at startup, so the same physical camera always lands in the same slot regardless of USB connection order
- Track labels now show the real model + GUID of the connected camera, or a placeholder ("Cam1"/"Cam2"/"Cam3"/"Cam4") when that slot has no camera yet
- New camera tracks appear automatically when an additional camera is detected beyond the number of existing tracks (up to 4 cameras)
- Fixed the misleading "network error" message shown under the location section - it wasn't a network failure at all, it just meant no IQP API key was configured. Now reads "To get IQP timings, set an API key in the Options menu"

### Alessandro Pessi
- Contact-time table (IQP/BE/Loc columns, all rows: C1/C2/Max/C3/C4/Rise/Set) now shows full HH:MM:SS.mmm precision instead of HH:MM - the underlying data was already millisecond-accurate, only the display was truncated to the minute (also reported independently by Alexandru Barbovschi)
- Added a tooltip on the "Paste Google Maps URL" field explaining that coordinates can also be typed directly into the Latitude/Longitude fields above it, without a link

## 2026-07-08 - Initial release
- First public, ready-to-run build: Sony camera control (Camera Remote SDK) for the total solar eclipse of August 12, 2026 (Burgos/Lerma, totality 103.9s). GUI + server + CLI, USB camera driver, 11,898-eclipse contact-time database (598 IANA time zones), voice-guided totality announcements (English/Polish), and example exposure sequences (JSON)
