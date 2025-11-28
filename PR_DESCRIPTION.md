# feature/auto-resize-v4l2 — Per‑Tile Offsets, Gap‑Support, Laufzeit‑Rotate/Flip, Steuerdatei + Async‑Screenshots + OCR

## Kurzüberblick
Dieser Branch erweitert hdmi_simple_display um:
- Pixelgenaue Ausgabe‑Offsets pro Kachel (150 Offsets),
- Laufzeitsteuerung zum Drehen und Spiegeln des Eingangsbildes,
- Konfigurierbare vertikale „Gaps“ zwischen Kachel‑Zeilen,
- Eine zentrale Steuerdatei `control_ini.txt`,
- Modul‑Dateinamen automatisch aus Seriennummern (`m<serial>.txt`) mit Fallback `modulN.txt`,
- Asynchrone Screenshot‑Funktion (Taste `s`) mit robustem YUV/packed Format‑Handling,
- OCR‑Integration: automatisches Erkennen einer Zahl im Screenshot (via Python/Tesseract) und Schreiben in `control_ini.txt` (z. B. `port = 324`), anschließend automatische Neuladung der Konfiguration und Upload der Uniforms,
- Umfangreiche Debug‑Logs für Format, Screenshot und OCR.

Alle langlaufenden oder blockierenden Operationen (PNG‑Kompression, OCR‑Aufruf, Dateisystemzugriffe) laufen asynchron, damit die Rendering/V4L2‑Loop nicht gestört wird.

---

## Neue / Wichtige Dateien
- `hdmi_simple_display.cpp` — enthält:
  - Loader für `control_ini.txt` inklusive `ocrPythonCmd` und `ocrRoi`.
  - Asynchronen Screenshot‑Worker (stb_image_write).
  - OCR‑Starter (popen, stdout/stderr abgefangen) + main‑thread Reload nach OCR‑Erfolg.
  - Modul‑Dateinamen via Seriennummern (m<serial>.txt).
- `ocr_config_updater.py` — Python‑Skript:
  - OpenCV + pytesseract basierte Erkennung einer Zahl im Bild.
  - Parameter: `--image`, `--config`, `--key`, `--roi`, `--debug`, usw.
  - Schreibt `key = <number>` in `control_ini.txt` und gibt die Zahl auf stdout zurück.
- `control_ini.txt` — zentrale Steuerdatei; neue Keys:
  - `ocrPythonCmd` — vollständiger Aufruf (z. B. `<venv>/bin/python /pfad/ocr_config_updater.py`).
  - `ocrRoi` — optional ROI `x,y,w,h` für OCR.

---

## How it works (Kurz)
1. Beim Start liest das Programm `control_ini.txt` (exe‑dir oder CWD). `modulN` Filenamen werden aus `modul1Serial/modul2Serial/modul3Serial` abgeleitet (m<serial>.txt) oder fallen auf `modulN.txt` zurück.
2. Taste `s`: zuletzt empfangener Frame wird asynchron in `display.png` geschrieben.
3. Nach erfolgreichem Write startet das Programm (sofern konfiguriert) asynchron das Python‑OCR via `ocrPythonCmd` (oder `OCR_PYTHON_CMD` env oder default) und übergibt `--image display.png --config control_ini.txt --key <port>` plus optional `--roi`.
4. Python‑Skript erkennt Zahl, schreibt `port = <num>` in `control_ini.txt` und gibt die Zahl auf stdout zurück.
5. C++ sieht den Erfolg, lädt `control_ini.txt` neu und updated Uniforms / Offsets (wie `k` Reload).

---

## Hotkeys
- s — Screenshot (asynchron) + OCR (wenn konfiguriert)
- k — Reload control_ini + modul files
- h — Toggle horizontal flip
- v — Toggle vertical flip
- r — Rotate (current implementation uses 180° steps)
- f — Fullscreen toggle
- ESC — Exit

---

## Konfigurationsbeispiele
control_ini.txt:
```
ocrPythonCmd = /mnt/ssd1/projects/hdmi-in-display/.venv-ocr/bin/python /mnt/ssd1/projects/hdmi-in-display/ocr_config_updater.py
ocrRoi = 600,200,720,400

fullInputSize = 3840,2160
segments = 3,3
subBlockSize = 1280,720
tileSize = 128,144
spacing = 98,90
numTiles = 10,15
modul1Serial = 1235976
modul2Serial = 2345987
modul3Serial = 3456123
```

---

## Debugging & Logs (was zu prüfen ist)
- Screenshot:
  - `[screenshot-worker] start filename=display.png ...`
  - `[screenshot-worker] saved: display.png`
- OCR:
  - `[ocr] launching OCR: ...`
  - `[ocr] OCR success, result='324'` oder `[ocr] OCR failed ... out='...'`
- Module/Offsets:
  - `Trying open offsets file: ...`
  - `Loaded offsetxy1 from module files (initial)`

---

## Empfehlungen / ToDos
- Falls die Zahl an bekannter Stelle immer auftaucht, setze `ocrRoi` → deutlich höhere Trefferquote.
- Optional: Timeout für OCR‑Aufruf (kann in C++ ergänzt werden).
- Optional: automatische Screenshot‑Dateinamen (Timestamp + erkannte Zahl).
- Falls du möchtest, kann ich die OCR retry/scale/filters im Python‑Skript erweitern (probiert mehrere Vorverarbeitungen automatisch).

---

Wenn du willst, commite ich alle Änderungen in den Branch `feature/auto-resize-v4l2`. Sag „commit“ und ich erstelle/texte den Commit mit Message:
```
Add OCR integration: ocrPythonCmd + ocrRoi, capture stderr, auto-reload; document INSTALL.md
```

Danke — sag mir kurz, ob ich committen soll oder noch weitere Dokumentanpassungen gewünscht sind.
