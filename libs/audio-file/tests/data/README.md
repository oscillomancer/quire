Test fixtures generated on 2026-09-26 with SoX/flac/LAME from a synthetic
0.25 s, 44.1 kHz stereo 440 Hz sine at -6 dBFS (no third-party audio content):

    sox -n -r 44100 -c 2 -b 16 sine440.wav synth 0.25 sine 440 vol 0.5
    flac -8 -o sine440.flac sine440.wav
    lame -b 128 sine440.wav sine440.mp3
