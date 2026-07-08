import requests
import os
import time

API_KEY = "sk_3c85634da6c4ca4d39a4150aad220a0b6c8a50e65e6a3c25"
VOICE_ID = "pNInz6obpgDQGcFmaJgB"  # Adam — zmień jeśli chcesz inny głos
MODEL_ID = "eleven_v3"
OUTPUT_DIR = "eclipse_audio"

os.makedirs(OUTPUT_DIR, exist_ok=True)

clips = [
    # PRE-C1
    ("01_pre_c1_10min",   "Partial eclipse begins in 10 minutes. Equipment check. Tracker, Both telescopes, Main camera, Secondary camera, Spectrograph, Three-sixty cameras, Batteries, Solar filters. All systems standing by."),
    ("02_pre_c1_5min",    "Partial eclipse begins in 5 minutes."),
    ("03_pre_c1_1min",    "Partial eclipse begins in 1 minute."),
    ("04_pre_c1_30s",     "Partial eclipse begins in 30 seconds."),
    ("05_pre_c1_15s",     "Partial eclipse begins in 15 seconds."),
    ("06_c1",             "Partial eclipse has begun. Moon has touched the Sun! "),

    # PARTIAL PHASE — every 5 min to C2
    ("07_to_c2_60min",    "Totality in 60 minutes. Watch the Moon moving across the Sun through your filters."),
    ("08_to_c2_55min",    "Totality in 55 minutes."),
    ("09_to_c2_50min",    "Totality in 50 minutes."),
    ("10_to_c2_45min",    "Totality in 45 minutes."),
    ("11_to_c2_40min",    "Totality in 40 minutes."),
    ("12_to_c2_35min",    "Totality in 35 minutes."),
    ("13_to_c2_30min",    "[commanding] Totality in 30 minutes. Important. If you need to use the bathroom — go NOW. Go now and come back. This is not a joke!"),
    ("14_to_c2_25min",    "[commanding] Totality in 25 minutes. Last chance for a bathroom break. After this — you stay here!"),
    ("15_to_c2_20min",    "Totality in 20 minutes. Everyone in position. Check your solar filters are secure. The sky will begin changing soon. Watch the light around you — it will go flat and strange."),
    ("16_to_c2_15min",    "Totality in 15 minutes. Look at the shadows. They are getting sharper. The light is already different. Animals may start acting strange. Temperature is dropping."),
    ("17_to_c2_10min",    "Totality in 10 minutes. The sky is darkening toward the west."),

    # PER MINUTE
    ("18_to_c2_9min",     "Totality in 9 minutes. Look west. The darkness on the horizon. That is the Moon's shadow - umbra. It is coming for us at two thousand kilometers per hour."),
    ("19_to_c2_8min",     "Totality in 8 minutes. The sky is changing color. Venus may already be visible. Look up — without filters — away from the Sun."),
    ("20_to_c2_7min",     "Totality in 7 minutes. Feel the temperature dropp. The wind may change."),
    ("21_to_c2_6min",     "Totality in 6 minutes. Cameras — check your settings one last time. Do it now while you still can think straight."),
    ("22_to_c2_5min",     "Totality in 5 minutes."),
    ("23_to_c2_4min",     "Totality in 4 minutes."),
    ("24_to_c2_3min",     "Totality in 3 minutes. The horizon to the west is going dark."),
    ("25_to_c2_2min",     "Totality in 2 minutes. Three hundred and sixty degree sunset is beginning - Look around you."),
    ("26_to_c2_1min",     "[commanding] ONE MINUTE TO TOTALITY. Filters stay ON. Get ready to take them off on my command!"),

    # FINAL SECONDS TO C2
    ("27_to_c2_45s",      "45 seconds. Stars are coming out in DAYLIGHT. The temperature is dropping."),
    ("28_to_c2_30s",      "30 seconds. Look at the ground without filters for SHADOW BANDS."),
    ("29_to_c2_20s",      "[commanding] Totality in 20 seconds. TAKE OFF YOUR SOLAR GLASSES AND FILTERS!"),
    ("30_to_c2_15s",      "15 seconds!"),
    ("31_to_c2_14s",      "14!"),
    ("32_to_c2_13s",      "13!"),
    ("33_to_c2_12s",      "12!"),
    ("34_to_c2_11s",      "11!"),
    ("35_to_c2_10s",      "10!"),
    ("36_to_c2_9s",       "9!"),
    ("37_to_c2_8s",       "8!"),
    ("38_to_c2_7s",       "7!"),
    ("39_to_c2_6s",       "6!"),
    ("40_to_c2_5s",       "5!"),
    ("41_to_c2_4s",       "4!"),
    ("42_to_c2_3s",       "3!"),
    ("43_to_c2_2s",       "2!"),
    ("44_to_c2_1s",       "1!"),

    # C2 — TOTALITY
    ("45_c2_totality",    "[shouting] TOTALITY!!! "),

    # TOTALITY — COUNTDOWN TO MAX
    ("46_to_max_50s",     "You have approximately 1 minute 44 seconds. Appreciated!"),
    ("47_to_max_40s",     "Maximum eclipse in 50 seconds."),
    ("48_to_max_30s",     "Maximum eclipse in 40 seconds."),
    ("49_to_max_20s",     "Maximum eclipse in 30 seconds."),
    ("50_to_max_10s",     "Maximum eclipse in 20 seconds."),
    ("51_to_max_now",     "Maximum eclipse in 10 seconds."),
    ("52_max_eclipse",    "[shouting] MAXIMUM ECLIPSE! Halfway through totality!"),

    # TOTALITY — COUNTDOWN TO C3
    ("53_to_c3_50s",      "End of totality in 50 seconds."),
    ("54_to_c3_40s",      "End of totality in 40 seconds."),
    ("55_to_c3_30s",      "End of totality in 30 seconds."),
    ("56_to_c3_20s",      "End of totality in 20 seconds."),
    ("57_to_c3_10s",      "End of totality in 10 seconds!"),
    ("58_to_c3_9s",       "9!"),
    ("59_to_c3_8s",       "8!"),
    ("60_to_c3_7s",       "7!"),
    ("61_to_c3_6s",       "6!"),
    ("62_to_c3_5s",       "5!"),
    ("63_to_c3_4s",       "4!"),
    ("64_to_c3_3s",       "3!"),
    ("65_to_c3_2s",       "2!"),
    ("66_to_c3_1s",       "1!"),

    # C3
    ("67_c3",             "End of totality! Baily's beads! Diamond ring!"),

    # POST-C3 FILTER WARNING
    ("68_post_c3_filters","[commanding] PUT ON YOUR SOLAR FILTERS. PUT ON YOUR SOLAR GLASSES."),

    # PARTIAL ECLIPSE — COUNTDOWN TO C4 every 10 min
    ("69_to_c4_60min",    "Partial eclipse ends in 60 minutes."),
    ("70_to_c4_50min",    "Partial eclipse ends in 50 minutes."),
    ("71_to_c4_40min",    "Partial eclipse ends in 40 minutes."),
    ("72_to_c4_30min",    "Partial eclipse ends in 30 minutes."),
    ("73_to_c4_20min",    "Partial eclipse ends in 20 minutes."),
    ("74_to_c4_10min",    "Partial eclipse ends in 10 minutes."),

    # FINAL 10s TO C4
    ("75_to_c4_10s",      "Partial eclipse ends in 10 seconds."),
    ("76_to_c4_9s",       "9!"),
    ("77_to_c4_8s",       "8!"),
    ("78_to_c4_7s",       "7!"),
    ("79_to_c4_6s",       "6!"),
    ("80_to_c4_5s",       "5!"),
    ("81_to_c4_4s",       "4!"),
    ("82_to_c4_3s",       "3!"),
    ("83_to_c4_2s",       "2!"),
    ("84_to_c4_1s",       "1!"),

    # C4
    ("85_c4_end",         "[excited] Partial eclipse is over. The eclipse has ended. Congratulations. Now you are one of us. See you in Egypt next year!"),
]


def generate_clip(filename, text):
    filepath = os.path.join(OUTPUT_DIR, f"{filename}.mp3")

    if os.path.exists(filepath):
        print(f"  SKIP (exists): {filename}.mp3")
        return True

    url = f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"
    headers = {
        "xi-api-key": API_KEY,
        "Content-Type": "application/json",
    }
    payload = {
        "text": text,
        "model_id": MODEL_ID,
        "voice_settings": {
            "stability": 0.35,
            "similarity_boost": 0.75,
            "style": 0.5,
            "use_speaker_boost": True
        }
    }

    response = requests.post(url, headers=headers, json=payload)

    if response.status_code == 200:
        with open(filepath, "wb") as f:
            f.write(response.content)
        print(f"  OK: {filename}.mp3")
        return True
    else:
        print(f"  ERROR {response.status_code}: {filename} — {response.text}")
        return False


print(f"Generating {len(clips)} clips into ./{OUTPUT_DIR}/\n")

success = 0
failed = 0

for i, (filename, text) in enumerate(clips):
    print(f"[{i+1}/{len(clips)}] {filename}")
    ok = generate_clip(filename, text)
    if ok:
        success += 1
    else:
        failed += 1
    time.sleep(0.5)  # grzeczny wobec API

print(f"\nDone. {success} OK, {failed} failed.")
print(f"Files saved to: ./{OUTPUT_DIR}/")