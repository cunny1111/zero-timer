# 0timer

simple cli tool to analyze audio files for bpm and offset. mostly built for osu! timing but works for whatever.

heavily inspired by statementreply's timing analyzer, but rewritten in c++ with some different heuristics and bias logic.

**note:** strictly for single-bpm songs. if your music drifts or has multi-bpm changes, this will probably fail. haven't mass tested it on everything so use at your own risk.

## architecture
just a simple visualization of the spagetti logic:
```
graph TD
    A[audio input] --> B[aubio analysis]
    B --> C{scouting phase}
    C -->|autocorrelation| D[rough bpm guess]
    D --> E[candidate generation]
    E --> F[grid scoring & voting]
    F -->|bias logic| G[final bpm]
    G --> H[zero-point projection]
    H --> I[output]
```
## how it works

the algo is a bit jank but here is the general pipeline:

1. **data collection:** uses `aubio` to grab spectral flux and onsets. nothing special here.
    
2. **scouting:** abuses autocorrelation on the flux data to find a "ballpark" bpm. basically finding where the signal overlaps with itself the most.
    
3. **the election (voting):**
    
    - takes the ballpark bpm and generates a list of integer candidates.
        
    - runs a "grid score" check on each candidate. it shifts the phase 20 times to see which bpm/offset combo aligns with the most onsets.
        
    - **bias:** i hardcoded some bias for common rhythm game tempos (170-190 bpm) and integers. it _can_ detect floating point bpms (like 160.45), but only if they score significantly higher than the nearest integer. otherwise, it snaps to int.
        
4. **zero-point projection:**
    
    - instead of trusting a single hit for offset, it builds a phase histogram from all onsets.
        
    - applies a fixed system latency compensation (-27ms, worked for my machine).
        
    - projects the offset backwards to the very start (t=0), so you always get a small, positive offset value within the first beat.
        

## building

- c++17
    
- requires `aubio` library (linked dynamically or statically, up to you).
    
- windows only (uses `windows.h` for the file dialog because i'm lazy).
    

just drop the code into a project, link aubio, and compile.

## usage

1. run `0timer.exe`
    
2. enter a manual bpm if you already know it and just want the offset (or leave blank to let the algo guess).
    
3. select your audio files (.mp3, .ogg, .wav).
    
4. profit.
    

## credits

- `aubio` for the heavy lifting on signal processing.
    
- `statementreply` for the original concept.