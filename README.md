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
```

### 🎶 Tokens

* **Mono:** `F[:ms]` → Frequenz in Hz, optional Dauer
* **Stereo:** `L,R[:ms]` → getrennte Frequenzen für links & rechts
* **Glide:** `A~B[:ms]` → linearer Übergang von Frequenz A nach B
* **Chord:** `f1+f2+...[:ms]` → mehrere Frequenzen gleichzeitig
* **Rest:** `r:ms` oder `0:ms` → Pause

### ⚙️ Global Options

* `-g` → Gain (0..1)
* `-sr` → Samplerate (default: 44100)
* `-l` → Default duration in ms
* `-fade` → Fade in/out in ms

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

```bash
gcc -o oabeep oabeep.c -lopenal -lm
```

Voraussetzungen installieren:

```bash
sudo apt install libopenal-dev
```

---

## 📜 Lizenz

GPLv3 – basierend auf freiem Code, erweitert um Sound-Sequenzen.

