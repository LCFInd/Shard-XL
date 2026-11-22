# SHARD XL

A sample mangler for industrial noise: eight tracks of samples cut into bands by a deck (2 to 4 bands, each with
its own level down to KILL, a shaper, DRIVE and feedback), a mixer with two buses through a crossfader, an FX
loop, a sequencer and scenes you jam between. It is the big brother of
[SHARD](https://github.com/LCFInd/SHARD), the firmware for the M-VAVE FM-1, and is headed for a Raspberry Pi 5 in
a box of its own. This first version runs on a Mac, to try the sound and the feel before any hardware.

**Status: an early test build.** It is built and its core is tested automatically on every change, but nobody
has played it on a Mac yet.

## Get it

1. Download `SHARD-XL-mac-<commit>.zip` from the release
   [mac-latest](https://github.com/LCFInd/Shard-XL/releases/tag/mac-latest) and unzip it.
2. Move **SHARD XL.app** to Applications (or anywhere).
3. The app is not signed by an Apple developer account, so the first time macOS refuses to open it. Open it once,
   then go to **System Settings › Privacy & Security**, scroll to the message about SHARD XL and choose
   **Open Anyway**. (Or, in Terminal: `xattr -dr com.apple.quarantine "/Applications/SHARD XL.app"`.)

It needs an Apple Silicon Mac (M1 or later) and macOS 12 or later.

## Play

On a first start it makes its own demo kit (drums, bass, metal, a drone, a machine loop, a stab) and three
scenes: press **Space** to play, click a scene on **SCENES** to switch at the next bar, hold it to morph.

- **Sound** comes out of the Mac's current output. **MIDI** from every controller plugged in is heard, the
  M-Audio Axiom included: its pads (channel 10) launch scenes 1 to 8, its keys play the selected track (or split
  them on SETUP), and SETUP › LEARN KNOBS maps its 8 knobs to the 8 on screen.
- **Samples**: drop WAV, AIFF, MP3, M4A, FLAC or CAF files on the window (on a track button to choose the
  track), or use the FILES page. Everything is converted to 48 kHz stereo.
- **Keys**: `?` shows them all. Space plays, 1 to 8 select a track, A W S E D F T G Y H U J K play notes,
  Shift+1-8 held kills that track's LOW band, Alt+1-4 held is a beat repeat, Cmd+K captures the last 60
  seconds, Cmd+M mutates, Cmd+Z undoes.
- **Files** live in `~/Music/SHARD-XL`: `Samples/`, `Captures/` (captures and resamples), `Projects/` (the
  project, saved every minute and on quit).

## The pages

| Page | What it does |
| --- | --- |
| WAVE | the sample, its slices (auto, even, or by hand), reverse, normalize, stretch to the tempo |
| DECK | the live spectrum in and out, crossovers to drag, each band's level as a red line, shaper and feedback per band, the DJ filter |
| MIX | 8 channel strips, sends to Bus A, Bus B and the FX loop, the crossfader, returns, master |
| SEQ | 8 tracks of up to 64 steps, each showing the slice or note it plays; drag them in the lane under the grid; chance, ratchets, "every Nth pass", Euclidean fills |
| SCENES | 8 scenes of every track's sound and pattern; launch on the bar, morph over 1 to 16 bars, or blend two by hand |
| FILES | browse and hear samples, load them into a track |
| SETUP | MIDI learning, the keyboard split, tempo, mutate amount, resample length |

## Build it yourself

On a Mac with the Xcode command line tools: `make app` builds `build/SHARD XL.app`; `make test` runs the core's
checks. On Linux, `make test` and `make shots` (every page as a picture, and 8 seconds of audio) work too.

## Licence

GPL-3.0-only (see LICENSE). The UI font is Inter Tight, under the SIL Open Font License (assets/fonts/OFL.txt).
