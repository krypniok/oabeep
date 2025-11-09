# oabeep – ein AI-gebauter `beep`-Nachfolger

oabeep ist ein vollständiger Ersatz für das historische Linux-`beep`. Die Idee stammt zwar aus einer nostalgischen Sehnsucht nach Hardware-Beep-Skripten, aber *der komplette Code, Parser, Synth und jede einzelne Erweiterung wurden zu 100 % mit GPT/Codex gebaut und iteriert*. Kein einziger C‑Block stammt aus einem bestehenden Projekt – wir haben die Maschine die Arbeit erledigen lassen.

Warum das wichtig ist? Das klassische `beep` kann nur piepsen, viele Distributionen haben es aus Sicherheitsgründen entfernt und moderne Workflows (Audio-Feedback, Sonifikation, Sounddesign) brauchen einen flexiblen Synth mit Sequencer, Samples, Text‑to‑Speech usw. Ein Tool wie oabeep fehlte schlicht – also hat Codex es implementiert.

Kurzüberblick:

* **Mono/Stereo/Glide/Chords/Rests**
* **Drums, Bass, Leads (KICK/SNARE/HAT/BASS/FLUTE/PIANO/GUITAR/EGTR)**
* **Samples per `WAV("file.wav")`**
* **CSV-Sampler mit BG/ADV, SAY-Events, Makros, Repeats**
* **Alles in einem OpenAL-Binary**

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
* **Melodic Instruments:** `FLUTE`, `PIANO`, `GUITAR`, `EGTR`, `BIRDS`, `STRPAD`, `BELL`, `BRASS`, `KALIMBA` (+ optional `@Note`/`@Hz`)
* **Samples:** `WAV("bells.wav")[:ms]` → bindet externe 16‑bit WAVs ein (mono oder stereo); Dauer optional (sonst Originalzeit)

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
  * Zusätzliche Synth-/Sample-Shortcuts: `KICK`, `SNARE`, `HAT`, `BASS`, `FLUTE`, `PIANO`, `GUITAR`, `EGTR`, `BIRDS`, `STRPAD`, `BELL`, `BRASS`, `KALIMBA`, `WAV("file.wav")`
* `duration_ms`: Pflicht, int
* `gap_ms`: optional Ruhe danach (ms oder Sekundenbruchteile), z.B. `0.25`
* `mode`: z.B. `GLIDE:300->1200`, `BINAURAL:7`, `UPx:1.2`. `|BG` oder `|ADV` können hier ebenfalls stehen.
* `flags`: Alternative Stelle für `BG` (Hintergrund, Timeline läuft weiter) oder `ADV` (BG blockt Timeline)

Sonderzeilen:

* `-SPAN,REPS` → wiederhole die **nächsten** `SPAN` Zeilen `REPS`‑mal (verschachtelt erlaubt)
* Kommentare beginnen mit `#`, `//` oder `--`

### 🎛️ Makros & Samples

Am Dateianfang (vor der eigentlichen Timeline) kannst du Symbole definieren:

```
@HYPERBASS {
BASS@45,800,60,BG|ADV
KICK@150->40,200,20,BG|ADV
SAY@de:hyperbass aktiviert!,0,0
}
```

Später reicht eine Zeile `@HYPERBASS,0,0,` und der Block wird inline expandiert (rekursiv möglich). In Makros dürfen alle Token verwendet werden – inkl. `SAY@`, `BG/ADV`, weiteren `@MACROS` sowie `WAV("sample.wav")`.

`WAV("…")` lädt externe 16‑bit PCM-WAVs (mono/stereo). Ohne explizite Dauer spielt oabeep das Sample in Originalgeschwindigkeit; mit `:ms` wird es streckt/gestaucht.

Sprachereignisse benötigen `espeak`. Bei Bedarf über `-espeak /pfad/zu/espeak` setzen.

Beispiel:

```
./oabeep -g 0.3 -f thunderstruck.txt
./oabeep -f muse.txt -espeak /usr/bin/espeak
./oabeep -g 0.28 -f flute_demo.txt
./oabeep -g 0.3 -f macro_demo.txt
```

Weitere Beispiele findest du in `flute_demo.txt`, `piano_demo.txt`, `guitar_demo.txt`, `egtr_demo.txt`, `macro_demo.txt` sowie `beat_demo.txt`.

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
