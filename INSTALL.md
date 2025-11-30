# Installation & Setup — hdmi-in-display (feature/auto-resize-v4l2)

Diese Datei beschreibt Schritt‑für‑Schritt, wie du das Projekt baust, notwendige System‑ und Python‑Abhängigkeiten installierst und die neue OCR‑Integration (ocr_config_updater.py) konfigurierst. Alles ist auf Linux (Debian/Ubuntu) ausgerichtet — für andere Systeme sind die Paketnamen / Befehle entsprechend anzupassen.

Wichtig: Lies zuerst die README/PR_DESCRIPTION für Feature‑Übersicht und Hotkeys.

---

Vorbedingungen (System)
- Ein modernes Linux (Debian/Ubuntu getestet)
- CMake (>= 3.10), make oder Ninja
- Build‑Werkzeuge: gcc/clang, make
- SDL2 dev, GLEW, OpenGL dev headers
- V4L2 (linux kernel), Zugriff auf /dev/video* (Das Programm verwendet V4L2 MMAP capture)

Debian/Ubuntu Beispiel‑Install (root / sudo):
```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libgl1-mesa-dev libglu1-mesa-dev libglew-dev libsdl2-dev \
    libv4l-dev libx11-dev libxcb1-dev libxrandr-dev libxi-dev \
    imagemagick   # optional, nur für ad-hoc crop/resize tests
```

stb_image_write
- Das Projekt verwendet die single‑header Bibliothek `stb_image_write.h`. Falls noch nicht im Repo, lade sie in den Projekt‑Root:
```bash
curl -L -o stb_image_write.h https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
```

Build (C++)
1. From project root:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```
2. Binary liegt dann in `build/hdmi_simple_display`.

Start:
```bash
./build/hdmi_simple_display
```

Stellen sicher, dass das V4L2 Device vorhanden ist (/dev/video0 oder passe die define im Code).

---

Python OCR Integration (ocr_config_updater.py)

Ziel: Tesseract/OpenCV OCR ausführen, erkannte Zahl in `control_ini.txt` schreiben (z. B. `port = 324`), und C++ Programm läd automatisch die neue Datei.

1) Python Virtualenv (empfohlen)
Im Projekt‑Root:
```bash
python3 -m venv .venv-ocr
source .venv-ocr/bin/activate
pip install --upgrade pip
pip install opencv-python pytesseract
```

2) System‑Tesseract (erforderlich)
Tesseract ist die native Engine, `pytesseract` ist ein Python‑Wrapper.
Debian/Ubuntu:
```bash
sudo apt update
sudo apt install -y tesseract-ocr
# optional: language packs
sudo apt install -y tesseract-ocr-deu
```

3) Skript ins Projekt kopieren
Lege `ocr_config_updater.py` in den Projekt‑Root (oder einen festen Pfad). Das Repo enthält bereits die Datei (siehe Projektdateien).

4) Testlauf (manuell)
Nachdem du einen Screenshot `display.png` erzeugt hast (Taste `s` im C++ Programm), teste das Skript manuell im venv:
```bash
source .venv-ocr/bin/activate
python3 ocr_config_updater.py --image display.png --config control_ini.txt --key port --debug
```
- Bei Erfolg: das Skript gibt die erkannte Zahl auf stdout und schreibt `port = <Zahl>` in `control_ini.txt`.
- Nutze `--roi x,y,w,h`, wenn die Zahl an bekannter Stelle ist (z. B. `--roi 600,200,720,400`) — ROI erhöht OCR‑Trefferquote deutlich.

---

Automatische Integration mit dem C++ Programm

Das C++ Programm startet das OCR‑Skript automatisch nach erfolgreichem Screenshot, wenn eine der folgenden Optionen gesetzt ist:

- Umgebungsvariable (Fallback)
  - OCR_PYTHON_CMD — z.B.
  ```bash
  export OCR_PYTHON_CMD="/voll/pfad/.venv-ocr/bin/python /voll/pfad/ocr_config_updater.py"
  ```

- control_ini.txt (empfohlen)
  - Setze in `control_ini.txt`:
    ```
    ocrPythonCmd = /mnt/ssd1/projects/hdmi-in-display/.venv-ocr/bin/python /mnt/ssd1/projects/hdmi-in-display/ocr_config_updater.py
    ocrRoi = 600,200,720,400    # optional, wird an das Skript gehängt
    ```
  - Wenn `ocrRoi` gesetzt ist, hängt das Programm `--roi <ocrRoi>` an den Aufruf an.

Das Programm ruft das Skript asynchron mittels popen() auf und fängt stdout/stderr ab (stderr wird jetzt umgeleitet und erscheint in den Programmlogs). Wenn das Skript Erfolg hat (Exit‑Code 0 und Ausgabe), lädt das C++ Programm `control_ini.txt` neu und wendet neue Parameter an (z. B. `port`).

---

Konfiguration: control_ini.txt
- Beispiel (voll):
```
ocrPythonCmd = /mnt/ssd1/projects/hdmi-in-display/.venv-ocr/bin/python /mnt/ssd1/projects/hdmi-in-display/ocr_config_updater.py
ocrRoi = 600,200,720,400

fullInputSize = 3840,2160
segments = 3,3
subBlockSize = 1280,720

tileSize = 128,144
spacing = 98,90
marginX = 0
numTiles = 10,15

inputTilesTopToBottom = 1

modul1Serial = 1235976
modul2Serial = 2345987
modul3Serial = 3456123
```
- `ocrPythonCmd` optional; falls leer, verwendet das C++ Programm `OCR_PYTHON_CMD` Umgebungsvariable oder den Default `python3 ocr_config_updater.py`.
- `ocrRoi` optional; Format `x,y,w,h` (Integer). Wird an `--roi` des Python Skripts übergeben.

---

Debugging Tipps
- Wenn OCR nichts findet: teste das Python‑Skript manuell mit `--debug` und ggf. `--roi`.
- Stelle sicher, dass `display.png` im CWD liegt oder gib absolute Pfade (C++ übergibt aktuell relative Pfade; du kannst `ocrPythonCmd` mit absoluten Pfad zur Scriptdatei setzen).
- Falls das C++ Programm keine Ausgabe zeigt: suche nach den Logzeilen:
  - `[screenshot-worker] saved: display.png`
  - `[ocr] launching OCR: ...`
  - `[ocr] OCR success, result='...'` oder `[ocr] OCR failed ... out='...'`
- Wenn Python Module fehlen: aktiviere venv, pip install die fehlenden Pakete.
- Wenn `tesseract` nicht gefunden: installiere system‑tesseract (apt/brew).

---

Optional / Erweiterungen
- Automatische Dateinamen für Screenshots (Timestamp + erkannte Zahl) — kann leicht aktiviert werden.
- Timeout fürs OCR‑Skript (kann in C++ ergänzt werden).
- Mehrfache ROIs / automatische Stichprobe im Python‑Skript (empfohlen für robuste Erkennung) — kann ich implementieren.

---

Support
Wenn du möchtest, dass ich die Änderungen committe und in deinen Branch pushe (feature/auto-resize-v4l2), sag „commit“ und ich erledige das. Wenn beim Testen Fehler auftreten, poste die Python (`--debug`) Logs und die relevanten C++ Konsolen‑Logs, ich helfe bei der Fehlersuche.
