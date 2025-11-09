# oabeep

**oabeep** ist ein OpenAL-basiertes Beep/Play-Utility mit erweiterten Features.
Es ersetzt das klassische `beep` durch einen flexiblen Sound-Synth mit:

* **Mono**
* **Stereo**
* **Glide (Frequenz-Slides)**
* **Chords (Akkorde)**
* **Rests (Pausen)**

---

## 🔧 Usage

```
oabeep [global options] token [token...]
oabeep -f sampler.csv [global options]
```

### 🎶 Tokens

* **Mono:** `F[:ms]` → Frequenz in Hz, optional Dauer
* **Stereo:** `L,R[:ms]` → getrennte Frequenzen für links & rechts
* **Glide:** `A~B[:ms]` → linearer Übergang von Frequenz A nach B
* **Chord:** `f1+f2+...[:ms]` → mehrere Frequenzen gleichzeitig
* **Rest:** `r:ms` oder `0:ms` → Pause
* **Synth-Percussion:** `KICK[:ms]`, `SNARE[:ms]`, `HAT[:ms]` → generierte Drums mit automatischer Hüllkurve
* **Bass:** `BASS[@freq][:ms]` oder `BASS(C2)` → saturierter Bass-Oszillator; Frequenz optional (Default 55 Hz)

### ⚙️ Global Options

* `-g` → Gain (0..1)
* `-sr` → Samplerate (default: 44100)
* `-l` → Default duration in ms
* `-fade` → Fade in/out in ms
* `-f` → CSV/Text-Sampler direkt abspielen (siehe unten)
* `-espeak` → Pfad zu `espeak`, falls SAY-Events genutzt werden

---

## 📄 Sampler-Dateien

Neben ad-hoc Tokens kann `oabeep` komplette `.txt/.csv`-Sequenzen rendern – identisch zu den früheren PHP-Skripten. Format:

```
token , duration_ms [, gap_ms] [, mode] [, flags]
```

* `token`: Frequenz (`440`), Note (`C4`, `A#3`), Akkord (`440+550+660`), Stereo (`"440,447"`), Glide (`300~900`), Pause (`0`/`R`) oder Sprachbefehl `SAY@voice;opts:text`
  * Zusätzliche Synth-Shortcuts: `KICK`, `SNARE`, `HAT`, `BASS` (+ optionale Parameter wie `BASS@42`, `KICK@180->40`)
* `duration_ms`: Pflicht, int
* `gap_ms`: optional Ruhe danach (ms oder Sekundenbruchteile), z.B. `0.25`
* `mode`: z.B. `GLIDE:300->1200`, `BINAURAL:7`, `UPx:1.2`. `|BG` oder `|ADV` können hier ebenfalls stehen.
* `flags`: Alternative Stelle für `BG` (Hintergrund, Timeline läuft weiter) oder `ADV` (BG blockt Timeline)

Sonderzeilen:

* `-SPAN,REPS` → wiederhole die **nächsten** `SPAN` Zeilen `REPS`‑mal (verschachtelt erlaubt)
* Kommentare beginnen mit `#`, `//` oder `--`

Sprachereignisse benötigen `espeak`. Bei Bedarf über `-espeak /pfad/zu/espeak` setzen.

Beispiel:

```
./oabeep -g 0.3 -f thunderstruck.txt
./oabeep -f muse.txt -espeak /usr/bin/espeak
```

---

## 🎼 Beispiele

```bash
# einfacher Ton 440 Hz, 200 ms
oabeep 440:200

# Stereo: 440 links, 660 rechts
oabeep 440,660:500

# Glide: von 220 Hz zu 880 Hz in 1 s
oabeep 220~880:1000

# Akkord: Dreiklang (C-Dur-artig), 800 ms
oabeep 440+550+660:800

# Pause 500 ms, dann Ton
oabeep r:500 440:300
```

---

## 🛠️ Build

```
gcc -o oabeep oabeep.c -lopenal -lm
```

Voraussetzungen installieren (`espeak` optional, nur für SAY):

```bash
sudo apt install libopenal-dev espeak
```

---

## 📜 Lizenz

GPLv3 – basierend auf freiem Code, erweitert um Sound-Sequenzen.
