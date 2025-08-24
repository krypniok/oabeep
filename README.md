# oabeep

`oabeep` ist ein OpenAL-basiertes Beep/Play-Utility mit erweiterten Features.  
Es ersetzt das simple `beep` durch flexiblen Sound-Synth mit **Mono, Stereo, Glide, Chords und Pausen**.

## Usage

```bash
oabeep [global opts] token [token...]

Tokens

Mono: F[:ms] → Frequenz in Hz, optional Dauer

Stereo: L,R[:ms] → getrennte Frequenzen für links & rechts

Glide: A~B[:ms] → linearer Übergang von Freq A nach Freq B

Chord: f1+f2+...[:ms] → mehrere Frequenzen gleichzeitig

Rest: r:ms oder 0:ms → Pause

Global Options

-g → Gain (0..1)

-sr → Samplerate (default 44100)

-l → Default duration in ms

-fade → Fade in/out in ms

Beispiele
# einfacher Ton 440Hz, 200ms
oabeep 440:200

# Stereo: 440 links, 660 rechts
oabeep 440,660:500

# Glide: von 220Hz zu 880Hz in 1s
oabeep 220~880:1000

# Akkord: Dur-Dreiklang 440Hz + 550Hz + 660Hz, 800ms
oabeep 440+550+660:800

# Pause 500ms, dann Ton
oabeep r:500 440:300

