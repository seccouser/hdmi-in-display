#!/usr/bin/env python3
"""
ocr_config_updater.py

Verwendet Tesseract (via pytesseract) und OpenCV, um aus einem Screenshot (z.B. display.png)
eine Zahl zu extrahieren und als key = value in eine INI-ähnliche Datei (control_ini.txt) zu schreiben.

Beispiel:
  python3 ocr_config_updater.py --image display.png --config control_ini.txt --key port

Optionen:
  --image PATH         Pfad zum Bild (required)
  --config PATH        Pfad zur control_ini.txt (default: control_ini.txt)
  --key KEY            INI-Key, der geschrieben werden soll (default: port)
  --min_value N        minimale akzeptierte Zahl (default: 1)
  --max_value N        maximale akzeptierte Zahl (default: 65535)
  --roi x,y,w,h        optionales ROI im Bild (Ganzzahlen) für gezielten OCR-Bereich
  --tesseract-cmd PATH optionaler Pfad zur tesseract binary
  --debug              ausführlichere Ausgaben
"""
import argparse
import sys
import os
import re
from typing import Optional, Tuple
import cv2
import pytesseract

class OCRUpdater:
    def __init__(self, tesseract_cmd: Optional[str] = None, debug: bool = False):
        if tesseract_cmd:
            pytesseract.pytesseract.tesseract_cmd = tesseract_cmd
        self.debug = debug

    def load_image(self, path: str):
        if self.debug:
            print(f"[OCR] load image: {path}", file=sys.stderr)
        img = cv2.imread(path, cv2.IMREAD_COLOR)
        if img is None:
            raise FileNotFoundError(f"Cannot load image: {path}")
        return img

    def preprocess(self, img, roi: Optional[Tuple[int,int,int,int]] = None):
        # roi: (x,y,w,h)
        if roi is not None:
            x,y,w,h = roi
            img = img[y:y+h, x:x+w]
            if self.debug:
                print(f"[OCR] using ROI {roi}", file=sys.stderr)
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        # adaptive or fixed threshold can help; try adaptive first, fall back to fixed
        thresh = cv2.adaptiveThreshold(gray, 255, cv2.ADAPTIVE_THRESH_MEAN_C,
                                       cv2.THRESH_BINARY, 15, 8)
        # also keep fixed threshold version for robustness
        _, fixed = cv2.threshold(gray, 150, 255, cv2.THRESH_BINARY)
        return thresh, fixed, gray

    def extract_candidate_numbers(self, img, roi=None, debug=False):
        thresh, fixed, gray = self.preprocess(img, roi)
        # try multiple variants and collect numeric matches
        candidates = []

        # config psm suitable for single line / single word numeric region:
        tesseract_config_digits = "--psm 7 -c tessedit_char_whitelist=0123456789"
        for method_name, source in (("adaptive", thresh), ("fixed", fixed), ("gray", gray)):
            try:
                txt = pytesseract.image_to_string(source, config=tesseract_config_digits)
            except Exception as e:
                if debug:
                    print(f"[OCR] tesseract error on {method_name}: {e}", file=sys.stderr)
                continue
            if not txt:
                continue
            txt = txt.strip()
            if debug:
                print(f"[OCR] raw ({method_name}): '{txt}'", file=sys.stderr)
            # keep only digits
            m = re.search(r"(\d{1,10})", txt)
            if m:
                candidates.append(int(m.group(1)))
        # also try more tolerant config (allow punctuation, then strip)
        try:
            txt2 = pytesseract.image_to_string(gray, config="--psm 6")
            if self.debug:
                print(f"[OCR] raw (psm6): '{txt2}'", file=sys.stderr)
            m2 = re.search(r"(\d{1,10})", txt2)
            if m2:
                candidates.append(int(m2.group(1)))
        except Exception:
            pass

        # deduplicate and sort by frequency (desc)
        if not candidates:
            return []
        freq = {}
        for v in candidates:
            freq[v] = freq.get(v, 0) + 1
        sorted_by_freq = sorted(freq.items(), key=lambda kv: (-kv[1], -kv[0]))
        return [kv[0] for kv in sorted_by_freq]

    def update_config_key(self, config_path: str, key: str, value: str):
        # Read file, update or append key = value preserving comments/other lines
        lines = []
        if os.path.exists(config_path):
            with open(config_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
        key_re = re.compile(rf'^\s*{re.escape(key)}\s*=')
        found = False
        out_lines = []
        for ln in lines:
            if key_re.match(ln):
                out_lines.append(f"{key} = {value}\n")
                found = True
            else:
                out_lines.append(ln)
        if not found:
            # append a newline if file doesn't end with newline
            if out_lines and not out_lines[-1].endswith("\n"):
                out_lines[-1] = out_lines[-1] + "\n"
            out_lines.append(f"{key} = {value}\n")
        with open(config_path, "w", encoding="utf-8") as f:
            f.writelines(out_lines)
        if self.debug:
            print(f"[OCR] wrote '{key} = {value}' to {config_path}", file=sys.stderr)

    def find_and_write(self, image_path: str, config_path: str, key: str,
                       min_value: int = 1, max_value: int = 65535,
                       roi: Optional[Tuple[int,int,int,int]] = None) -> Optional[int]:
        img = self.load_image(image_path)
        candidates = self.extract_candidate_numbers(img, roi=roi, debug=self.debug)
        if self.debug:
            print(f"[OCR] candidates: {candidates}", file=sys.stderr)
        for num in candidates:
            if num >= min_value and num <= max_value:
                # found acceptable
                self.update_config_key(config_path, key, str(num))
                return num
        # no valid candidate found
        return None

def parse_args():
    p = argparse.ArgumentParser(description="OCR image -> update config key with extracted number")
    p.add_argument("--image", "-i", required=True)
    p.add_argument("--config", "-c", default="control_ini.txt")
    p.add_argument("--key", "-k", default="port")
    p.add_argument("--min", dest="min_value", type=int, default=1)
    p.add_argument("--max", dest="max_value", type=int, default=65535)
    p.add_argument("--roi", help="ROI x,y,w,h (integers)", default=None)
    p.add_argument("--tesseract-cmd", help="Path to tesseract binary (optional)", default=None)
    p.add_argument("--debug", action="store_true")
    return p.parse_args()

def main():
    args = parse_args()
    roi = None
    if args.roi:
        try:
            parts = [int(x) for x in args.roi.split(",")]
            if len(parts) == 4:
                roi = tuple(parts)
        except Exception:
            print("Invalid ROI. Use x,y,w,h", file=sys.stderr)
            sys.exit(2)
    updater = OCRUpdater(tesseract_cmd=args.tesseract_cmd, debug=args.debug)
    try:
        res = updater.find_and_write(args.image, args.config, args.key,
                                     min_value=args.min_value, max_value=args.max_value, roi=roi)
    except FileNotFoundError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
    if res is None:
        if args.debug:
            print("[OCR] no valid numeric candidate found", file=sys.stderr)
        print("")  # nothing to stdout for easy caller detection
        sys.exit(1)
    else:
        print(str(res))  # print found number
        sys.exit(0)

if __name__ == "__main__":
    main()
