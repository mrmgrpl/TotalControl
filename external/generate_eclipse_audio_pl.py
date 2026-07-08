import requests
import os
import time

API_KEY = "PUT YOU API KEY IN THIS PLACE"
VOICE_ID = "hIssydxXZ1WuDorjx6Ic"  # Adam — zmień na polski głos jeśli chcesz
MODEL_ID = "eleven_v3"
OUTPUT_DIR = "eclipse_audio_pl"

os.makedirs(OUTPUT_DIR, exist_ok=True)

clips = [
    # PRE-C1
    ("01_pre_c1_10min",   "Częściowe zaćmienie rozpoczyna się za 10 minut. Kontrola sprzętu. Tracker, oba teleskopy, główny aparat, drugi aparat, spektrograf, kamery trzysta sześćdziesiąt stopni, baterie, filtry słoneczne. Wszystkie systemy gotowe."),
    ("02_pre_c1_5min",    "Częściowe zaćmienie rozpoczyna się za 5 minut."),
    ("03_pre_c1_1min",    "Częściowe zaćmienie rozpoczyna się za 1 minutę."),
    ("04_pre_c1_30s",     "Częściowe zaćmienie rozpoczyna się za 30 sekund."),
    ("05_pre_c1_15s",     "Częściowe zaćmienie rozpoczyna się za 15 sekund."),
    ("06_c1",             "Częściowe zaćmienie rozpoczęte - księżyc dotknął słońca!"),

    # FAZA CZĘŚCIOWA — co 5 minut do C2
    ("07_to_c2_60min",    "Zaćmienie całkowite za 60 minut."),
    ("08_to_c2_55min",    "Zaćmienie całkowite za 55 minut."),
    ("09_to_c2_50min",    "Zaćmienie całkowite za 50 minut."),
    ("10_to_c2_45min",    "Zaćmienie całkowite za 45 minut."),
    ("11_to_c2_40min",    "Zaćmienie całkowite za 40 minut."),
    ("12_to_c2_35min",    "Zaćmienie całkowite za 35 minut."),
    ("13_to_c2_30min",    "[commanding] Zaćmienie całkowite za 30 minut. Uwaga. Jeśli musisz skorzystać z toalety — idź TERAZ. Idź i wróć. To nie jest żart!"),
    ("14_to_c2_25min",    "[commanding] Zaćmienie całkowite za 25 minut. Ostatnia szansa na toaletę. Po tym — zostajesz tutaj."),
    ("15_to_c2_20min",    "Zaćmienie całkowite za 20 minut. Wszyscy na pozycjach."),
    ("16_to_c2_15min",    "Zaćmienie całkowite za 15 minut. Spójrz na cienie, robią się ostrzejsze. Światło jest już inne. Zwierzęta mogą zacząć się dziwnie zachowywać. Temperatura spada."),
    ("17_to_c2_10min",    "Zaćmienie całkowite za 10 minut. Niebo ciemnieje na zachodzie."),

    # CO MINUTĘ
    ("18_to_c2_9min",     "Zaćmienie całkowite za 9 minut. Spójrz na zachód - ciemność na horyzoncie to prawdziwy cień Księżyca — umbra. Zbliża się do nas z prędkością dwóch tysięcy kilometrów na godzinę."),
    ("19_to_c2_8min",     "Zaćmienie całkowite za 8 minut. Niebo zmienia kolor. Wenus może być już widoczna. Unikaj spogladania na słońce bez filtra!"),
    ("20_to_c2_7min",     "Zaćmienie całkowite za 7 minut. Czujesz spadek temperatury? Wiatr może się zmienić."),
    ("21_to_c2_6min",     "Zaćmienie całkowite za 6 minut. Aparaty — sprawdź ustawienia po raz ostatni. Zrób to teraz, dopóki jeszcze myślisz logicznie."),
    ("22_to_c2_5min",     "Zaćmienie całkowite za 5 minut.  "),
    ("23_to_c2_4min",     "Zaćmienie całkowite za 4 minuty. "),
    ("24_to_c2_3min",     "Zaćmienie całkowite za 3 minuty. "),
    ("25_to_c2_2min",     "Zaćmienie całkowite za 2 minuty. Zachód słońca trzysta sześćdziesiąt stopni właśnie się zaczyna — spójrz dookoła."),
    ("26_to_c2_1min",     "[commanding] MINUTA DO ZAĆMIENIA CAŁKOWITEGO! Bądź gotowy zdjąć filtry na mój znak!"),

    # OSTATNIE SEKUNDY DO C2
    ("27_to_c2_45s",      "[excited] 45 sekund. Gwiazdy wychodzą w ciągu dnia. Temperatura spada. Ptaki milkną."),
    ("28_to_c2_30s",      "[excited] 30 sekund. Spójrz na ziemię bez filtrów — PASY CIENIA. Falujące, migoczące smugi światła."),
    ("29_to_c2_20s",      "[commanding] Zaćmienie całkowite za 20 sekund. ZDEJMIJ OKULARY SŁONECZNE! PATRZ NA SŁOŃCE BEZ OKULARÓW. ZDEJMIJ FILTRY Z APARATÓW!"),
    ("30_to_c2_15s",      "Zaćmienie całkowite za 15 sekund!"),
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

    # C2 — ZAĆMIENIE CAŁKOWITE
    ("45_c2_totality",    "[shouting] ZAĆMIENIE CAŁKOWITE!"),

    # ZAĆMIENIE CAŁKOWITE — ODLICZANIE DO MAKSIMUM
    ("46_to_max_50s",     "Masz około minuty i 44 sekund. Podziwiaj!"),
    ("47_to_max_40s",     "Maksimum zaćmienia za 50 sekund."),
    ("48_to_max_30s",     "Maksimum zaćmienia za 40 sekund."),
    ("49_to_max_20s",     "Maksimum zaćmienia za 30 sekund."),
    ("50_to_max_10s",     "Maksimum zaćmienia za 20 sekund."),
    ("51_to_max_now",     "Maksimum zaćmienia za 10 sekund."),
    ("52_max_eclipse",    "[shouting] MAKSIMUM! Jesteśmy połowie zaćmienia całkowitego!"),

    # ZAĆMIENIE CAŁKOWITE — ODLICZANIE DO C3
    ("53_to_c3_50s",      "Koniec zaćmienia całkowitego za 50 sekund."),
    ("54_to_c3_40s",      "Koniec zaćmienia całkowitego za 40 sekund."),
    ("55_to_c3_30s",      "Koniec zaćmienia całkowitego za 30 sekund."),
    ("56_to_c3_20s",      "Koniec zaćmienia całkowitego za 20 sekund."),
    ("57_to_c3_10s",      "Koniec zaćmienia całkowitego za 10 sekund!"),
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
    ("67_c3",             "Koniec zaćmienia całkowitego! Paciorki Baileya! Pierścień diamentowy!"),

    # OSTRZEŻENIE PO C3
    ("68_post_c3_filters","[commanding] ZAŁÓŻ FILTRY SŁONECZNE! ZAŁÓŻ OKULARY SŁONECZNE!"),

    # FAZA CZĘŚCIOWA — ODLICZANIE DO C4 co 10 minut
    ("69_to_c4_60min",    "Częściowe zaćmienie kończy się za 60 minut."),
    ("70_to_c4_50min",    "Częściowe zaćmienie kończy się za 50 minut."),
    ("71_to_c4_40min",    "Częściowe zaćmienie kończy się za 40 minut."),
    ("72_to_c4_30min",    "Częściowe zaćmienie kończy się za 30 minut."),
    ("73_to_c4_20min",    "Częściowe zaćmienie kończy się za 20 minut."),
    ("74_to_c4_10min",    "Częściowe zaćmienie kończy się za 10 minut."),

    # OSTATNIE 10 SEKUND DO C4
    ("75_to_c4_10s",      "Częściowe zaćmienie kończy się za 10 sekund!"),
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
    ("85_c4_end",         "[excited] Częściowe zaćmienie dobiegło końca. Zaćmienie zakończone. Gratulacje. Teraz jesteś jednym z nas. Do zobaczenia w Egipcie za rok!"),
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
