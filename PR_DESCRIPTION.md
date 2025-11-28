# feature/auto-resize-v4l2 — Per‑Tile Offsets, Gap‑Support, Laufzeit‑Rotate/Flip, Steuerdatei + Async‑Screenshots

## Kurzüberblick
Dieser Branch erweitert hdmi_simple_display um:
- Pixelgenaue Ausgabe‑Offsets pro Kachel (150 Offsets),
- Laufzeitsteuerung zum Drehen und Spiegeln des Eingangsbildes,
- Konfigurierbare vertikale „Gaps“ zwischen Kachel‑Zeilen,
- eine konfigurierbare Steuerdatei `control_ini.txt` (inkl. Modul‑Seriennummern),
- automatisches Ableiten der Modul‑Dateinamen aus Seriennummern (`m<serial>.txt`),
- asynchrone Screenshot‑Funktion (Taste `s`) mit Unterstützung verbreiteter YUV/packed Formate,
- Live‑Reload der Parameter/Offsets (Taste `k`) und ausführliche Debug‑Logs.

Alle Änderungen wurden so umgesetzt, dass die normale Rendering/V4L2‑Loop nicht durch Screenshot‑Erstellung oder Reload blockiert wird.

---

## Neu: Modul‑Dateinamen basierend auf Seriennummern
- In `control_ini.txt` werden die Seriennummern der Module als drei Einträge definiert:
  - modul1Serial = 1235976
  - modul2Serial = 2345987
  - modul3Serial = 3456123
- Aus diesen Werten erzeugt das Programm die Modul‑Dateinamen:
  - m1235976.txt, m2345987.txt, m3456123.txt
- Verhalten:
  - Beim Start und beim Reload (Taste `k`) wird `control_ini.txt` gelesen und daraus `modFiles` erzeugt.
  - Die Loader‑Routine sucht für jede Moduldatei zuerst im Executable‑Verzeichnis (exe dir) und danach im aktuellen Arbeitsverzeichnis (CWD).
  - Falls eine Seriennummer den Wert `0` hat (Default), wird als Fallback weiterhin `modul1.txt` / `modul2.txt` / `modul3.txt` erwartet — so bleibt die Lösung abwärtskompatibel.
- Du musst nun also sicherstellen, dass die Dateien mit den Namen `m<serial>.txt` an einem der beiden geprüften Orte vorhanden sind.

---

## Steuerdatei `control_ini.txt` (erweitert)
- Zweck: zentrale Konfiguration für Shader/Grid/Module.
- Unterstützte Keys (key = value; Kommentare mit `#`):
  - fullInputSize = W,H
  - segments = X,Y
  - subBlockSize = W,H
  - tileSize = W,H
  - spacing = X,Y
  - marginX = N
  - numTiles = cols,rows
  - inputTilesTopToBottom = 0|1
  - modul1Serial = 1235976
  - modul2Serial = 2345987
  - modul3Serial = 3456123
- Zusätzlich wird weiterhin die legacy‑Form gelesen:
  - moduleSerials = 1235976,2345987,3456123
  - Falls sowohl legacy als auch die neuen Einzel‑Keys vorhanden sind, haben die Einzel‑Keys Vorrang.
- Beispiel:
  ```
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

---

## Per‑Tile Offsets und Moduldateien
- Offsets werden über drei Dateien geladen: standardmäßig die aus Seriennummern abgeleiteten `m<serial>.txt` (oder fallback `modulN.txt`).
- Format pro Datei: bis zu 50 Zeilen, jede Zeile zwei ints:
  ```
  <x> <y>
  ```
  Leerzeilen und `#`‑Kommentare werden ignoriert. Fehlende Einträge werden mit `0,0` aufgefüllt.
- Die gelesenen 150 (3×50) Paare werden per `glUniform2iv(loc_offsetxy1, 150, data)` ans Fragment‑Shader übergeben.

---

## Gap‑Support (vertikale Nähte)
- Uniforms:
  - gap_count — Anzahl gültiger Einträge
  - gap_rows[8] — 1‑basierte Zeilennummern; an diesen Stellen wird der vertikale spacing auf 0 gesetzt.
- Default im Code: z. B. `{5,10}` zur Modellierung von Display‑Nähten.
- Werte sind 1‑basiert und dürfen im Bereich `1 .. (numTilesPerCol-1)` liegen.

---

## Laufzeitsteuerungen (Hotkeys)
- s — Screenshot (asynchron; Ergebnis: `display.png` im CWD)
- k — Reload: liest `control_ini.txt` und die Moduldateien neu, lädt Offsets in Shader
- h — Toggle horizontales Spiegeln (flip_x)
- v — Toggle vertikales Spiegeln (flip_y)
- r — Rotation (aktuell 180° steps; kann auf 90° pro Druck umgestellt werden)
- f — Fullscreen toggle (SDL)
- ESC / Fenster schließen — Beenden

---

## Screenshot‑Funktion (Technik & Verhalten)
- Beim Empfang jedes Frames werden minimal nötige Rohdaten zwischengespeichert:
  - NV12/NV21: lastY + lastUV
  - Andere / single‑plane: lastPacked (komplettes Frame‑Blob), last_pixfmt, last_width/last_height
- Beim Drücken von `s`:
  - Relevante Puffer werden kopiert und an einen detached `std::thread` übergeben.
  - `async_save_frame_to_png(...)` dekodiert abhängig vom Format:
    - NV12/NV21: direkte Y/UV‑Dekodierung (uv_swap berücksichtigt)
    - Heuristik: wenn `packedSize == Y_len + UV_len` → Split packed→Y/UV und dekodieren als NV12‑like
    - Packed 4:2:2: YUYV / UYVY → korrekt dekodiert
    - Fallback: rohes RGB wenn Byte‑Größe passt
  - PNG wird mit `stb_image_write` geschrieben (kein externes Tool nötig).
- Worker‑Logs: `[screenshot-worker] start ...` / `[screenshot-worker] saved: display.png` bzw. Fehlernachrichten.
- Performance: Hauptloop bleibt ungebremst; CPU‑Arbeit + Datei‑I/O läuft asynchron.

---

## Shader‑Uniforms (kompakt)
- sampler2D texY, texUV
- ivec2 offsetxy1[150]
- int segmentIndex
- vec2 u_fullInputSize
- int u_segmentsX, u_segmentsY
- vec2 u_subBlockSize
- float u_tileW, u_tileH, u_spacingX, u_spacingY, u_marginX
- int u_numTilesPerRow, u_numTilesPerCol
- int rot, flip_x, flip_y
- int gap_count, gap_rows[8]
- int inputTilesTopToBottom
- int moduleSerials[3]
- int uv_swap, full_range, use_bt709, view_mode

---

## Build & Test (Kurz)
1. Build:
   ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```
2. `control_ini.txt` anpassen (insbesondere modul1Serial/2/3).
3. Modul‑Dateien erzeugen/platzieren (Beispiel):
   - m1235976.txt, m2345987.txt, m3456123.txt in exe‑dir oder CWD
4. Start:
   ```
   ./build/hdmi_simple_display
   ```
5. Interaktion:
   - `k` : Reload control + modulfiles (falls du die Moduldateien geändert hast)
   - `s` : Screenshot schreiben (asynchron)
   - `h`, `v`, `r`, `f`, `ESC` wie oben

---

## Debugging Hinweise
- Beim Start werden Suchpfade für Module geloggt:
  - z. B. `Trying open offsets file: /path/to/build/m1235976.txt` (exe dir)
  - Falls nicht gefunden: `Trying open offsets file: m1235976.txt` (CWD)
- Wenn Offsets korrekt geladen wurden: `Loaded offsetxy1 from module files (initial)` (mit Beispiel‑Werten).
- Screenshot‑Debug: `s pressed: last_pixfmt=... (FOURCC) lastPacked=... last_w=... last_h=...` und `[screenshot-worker] ...`.
- Falls eine Uniform im Shader optimiert wurde (Location == -1), wird eine Warnung geloggt; im aktuellen Branch sind die Uniforms aktiv genutzt, daher üblicherweise sichtbar.

---

## Hinweise & ToDos / Erweiterungen (optional)
- Automatische Dateinamen für Screenshots (Timestamp + modulSerial) — kann leicht ergänzt werden.
- FBO‑FullInput‑Screenshot (render FBO in fullInputSize + glReadPixels) — empfohlen, wenn GPU/Textur bereits das komplette FullInput enthält.
- Optionales konfigurierbares Modul‑Prefix (statt hart `"m"`) via `control_ini.txt`.
- UI/OSD Bestätigung nach Screenshot (kleine On‑Screen‑Meldung).
- Größere `gap_rows` Array‑Länge falls mehr Gaps nötig sind (aktuell 8).

---

## PR / Commit Hinweise
- Vorschlag Commit‑Message:
  ```
  Add per-tile offsets, gap support, runtime rotate/flip, control_ini with module serials and async screenshot + module filenames from serials
  ```
- PR‑Titel:
  ```
  feature/auto-resize-v4l2: per‑tile offsets + gap handling + runtime rotate/flip + screenshots + module filename by serial
  ```

---

Wenn du möchtest, übernehme ich:
- Commit & Push in `feature/auto-resize-v4l2` mit obigen Änderungen,
- zusätzlich: automatische Screenshot‑Dateinamen (Timestamp + modul1Serial),
- oder: FBO‑basierte FullInput‑Screenshot‑Implementierung — sag welche Variante du willst.
