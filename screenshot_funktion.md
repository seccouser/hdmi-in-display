# Screenshot-Funktion (Deutsch) — hdmi_simple_display

Diese Datei beschreibt die neu hinzugefügte Screenshot‑Funktionalität in hdmi_simple_display (Branch: feature/auto-resize-v4l2). Sie erklärt kurz, wie die Funktion arbeitet, welche Formate unterstützt werden, welche Tasten es gibt und wie du Vollbild‑Screenshots (Full Input / 3840×2160) realisieren kannst.

Kurzüberblick
- Taste `s`: erzeugt asynchron eine PNG-Datei `display.png` im aktuellen Arbeitsverzeichnis (worker schreibt mit stb_image_write, blockiert nicht die Anzeige).
- Unterstützte Eingabe‑Formate:
  - NV12 / NV21 (Y plane + interleaved UV plane) — direkt unterstützt.
  - Contiguous NV12-like (Y gefolgt von UV in einem einzigen Puffer) — heuristische Erkennung und Split in Y/UV.
  - Packed 4:2:2: YUYV und UYVY — werden korrekt dekodiert.
  - Falls das gepackte Puffervolumen RGB-Daten enthält, wird es als RGB übernommen.
- Debug-Logs: Bei Screenshot‑Request werden Informationen geloggt (FourCC, Buffer‑Größen, Auflösung). Worker loggt Start/Ende (z. B. `[screenshot-worker] saved: display.png`).

Technische Details (Kurz, Deutsch)
- Beim Empfang eines Frames speichern wir für Screenshots:
  - Für NV12/NV21: lastY (Y plane) und lastUV (interleaved UV).
  - Für andere Layouts: lastPacked (komplettes Frame‑Blob), last_pixfmt, last_width/last_height.
- Wenn `s` gedrückt wird:
  - Die relevanten Puffer werden kopiert und in einem detached std::thread an die Funktion `async_save_frame_to_png(...)` übergeben.
  - `async_save_frame_to_png` entscheidet abhängig von last_pixfmt bzw. Heuristiken welche Decode‑Routine verwendet wird:
    - NV12/NV21 (oder heuristisch erkannte Y+UV‑Contiguous → Split),
    - YUYV/UYVY (Packed 4:2:2) oder
    - Roh‑RGB (falls Größe passt).
  - YUV→RGB Umrechnung verwendet dieselben Konstanten wie der Shader (BT.709/BT.601, limited/full range).
  - PNG wird mit stb_image_write (`stbi_write_png`) geschrieben.
- Performance: die Anzeige‑/Rendering‑Schleife bleibt ungehindert, weil die teure Konvertierung/Komprimierung asynchron ausgeführt wird. Die einzige Laufzeitkosten sind die einmaligen memcpy‑Kopien beim Anstoßen des Screenshots.

Dateiname / Pfad / Timestamp / Serials
- Aktuell: `display.png` im aktuellen Arbeitsverzeichnis.
- Du kannst den Dateinamen leicht ändern, indem du beim Aufruf von `async_save_frame_to_png` den gewünschten String übergibst. Vorschlag für automatische Datei‑Namen:
  - `display_<YYYYMMDD_HHMMSS>_<serial>.png` — dabei kannst du die im `control_ini.txt` eingelesenen Modul‑Seriennummern (moduleSerials[3]) verwenden.
- Wenn du möchtest, kann ich das Programm so erweitern, dass beim Schreiben automatisch ein Timestamp und die erste Seriennummer in den Dateinamen eingebaut wird.

Hotkeys (Übersicht)
- s — Screenshot (asynchron).
- k — Nachladen: modul*.txt Offsets und control_ini.txt neu einlesen (live reload).
- h — flip_x toggle (horizontal mirror).
- v — flip_y toggle (vertical flip).
- r — rotation (180° steps, in aktuellem Build).
- f — Toggle Fullscreen (SDL).
- ESC / Fenster schließen — Beenden.

control_ini.txt (Kurz)
- Steuerdatei (z. B. im gleichen Verzeichnis wie Binary). Beispiel:
```
fullInputSize = 3840,2160
segments = 3,3
subBlockSize = 1280,720

tileSize = 128,144
spacing = 98,90
marginX = 0
numTiles = 10,15

inputTilesTopToBottom = 1

moduleSerials = 1235976,2345987,3456123
```
- Die Datei wird beim Start und beim Drücken von `k` neu geladen. Module‑Seriennummern werden in `moduleSerials` als Uniform an den Shader übergeben (du kannst sie im Shader für OSD verwenden).

Wenn du ein FullInput (3840×2160) Screenshot möchtest
Es gibt zwei mögliche Wege — wähle einen:

1) Offscreen‑FBO Screenshot (empfohlen, wenn die GPU/Shader bereits das ganze Input‑Bild im Texturformat hat)
   - Idee: rendere (falls möglich) die Quell‑Textur(en) oder ein spezielles Full‑Input‑Quad in eine FBO mit Größe 3840×2160 und rufe glReadPixels auf → Puffer → PNG (ebenfalls asynchron in Worker).
   - Vorteile: exakt das, was die GPU liefert; kein Stitching; schnell(er) als CPU‑Stitch.
   - Aufwand: kleine GL‑Ergänzung (Erzeugen eines FBO, Binden als RenderTarget und ein DrawCall), danach glReadPixels() in einen Puffer und Übergabe an den PNG‑Worker.

2) Stitching aller Segmente (falls Gerät nur Subblocks liefert)
   - Idee: wenn das Gerät nur Subblocks (z. B. 9 Segmente à 1280×720) liefert, muss das Programm jeweils den letzten Frame jedes Segments sammeln und dann die 3×3 Blöcke zusammensetzen zu 3840×2160.
   - Nachteile: aufwendiger (mehr Puffer, Synchronisation), langsamer (mehr memcpy und CPU‑Stitching), aber funktioniert auch, wenn der Hardware‑Stream nicht ein zusammenhängendes FullFrame liefert.

Ich kann die FBO‑Variante sofort einbauen, sofern:
- die Fragment/Vertex‑Pipeline beim Start Zugriff auf die vollständige Quelltextur hat (z. B. u_fullInputSize in Shader und die Quelltextur enthält das FullInput). Das ist in deinem Build bereits teilweise vorbereitet (Uniforms `u_fullInputSize` und Subblock‑Mapping).
- Alternativ implementiere ich Stitching, falls du sicher weißt, dass die Hardware nur Subblocks liefert.

Fehlerbehebung (wenn Screenshot nicht erstellt wird)
- Prüfe die Logs (stderr): vor allem Zeilen mit `s pressed:` und `[screenshot-worker]`.
- Wenn Meldung lautet `No last NV12/NV21 frame available` → bedeutet, dass noch kein Frame in den erwarteten Format‑Puffern gespeichert wurde. Warte auf erste Frames oder prüfe `cur_pixfmt` mittels Log.
- Wenn `unsupported pixfmt` erscheint → melde die FourCC‑Bezeichnung (wird geloggt) und ich erweitere den Worker um genau diesen Fall.

Mögliche Erweiterungen (falls du willst)
- Dateinamen mit Timestamp / Seriennummern automatisch.
- Screenshot‑Ordner (configbar).
- On‑screen‑Benachrichtigung (kurze OSD Meldung "Screenshot gespeichert").
- Konfiguration via control_ini.txt (z. B. screenshot_enabled, screenshot_dir, screenshot_fullsize=1).
- Commit & Push der Änderungen in feature/auto-resize-v4l2 (auf Wunsch erledige ich das).

Wenn du möchtest, füge ich die Funktion „FullInput FBO Screenshot“ sofort ein (ich implementiere es so, dass es nur aktiviert wird, wenn `u_fullInputSize` größer ist als current capture size und die GL Textur tatsächlich enthält, was wir beim Start prüfen). Sag kurz: FBO‑Variante oder Stitching? Ebenfalls: möchtest du automatische Dateinamen (Timestamp + Serial) oder `display.png` reicht?  
