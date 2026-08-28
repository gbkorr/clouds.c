# clouds.c
A somewhat feature-rich volumetric cloud renderer for the terminal, intended as a fancy alternative to cbonsai/cmatrix. Code by Claude Fable; aesthetic and UX decisions by me. I know what the code does, but I'm not good at writing C :P

handwritten [writeup](https://gbkorr.github.io/r-bites/clouds/clouds.html)


## Installation
```
git clone https://github.com/gbkorr/clouds.git
cd clouds/clouds/
make
```
(Optional: add to PATH, e.g. `sudo mv clouds /usr/bin/`, will be callable anywhere once terminal is reopened)

## Usage
```
Usage: clouds [OPTIONS]
Use the arrow keys to look around.

volumetric clouds~

Presets: flat below thunder sol nimbus
Times: noon afternoon golden sunset storm

  -P, --preset NAME    use a preset (listed above)
  -c, --coverage F     cloud amount (default 0.9) [0,2]
  -H, --height F       cloud size (default 4.0) [0.5,6]
  -t, --time NAME      time/weather for backdrop (listed above)
  -z, --azimuth DEG    sun azimuth in degrees (default 30) [0,360]
  -w, --wind F         wind/cloud speed (default 1.0)
  -e, --elevation F    camera elevation; [0,1] below/above clouds (default 0.6)
  -y, --yaw DEG        camera yaw (default 0) [0,360]
  -p, --pitch DEG      camera pitch [-89,89]
  -q, --quality Q      raymarch steps (default 40), beware CPU cost! [10,512]
  -F, --fps N          target fps (default 24) 
  -C, --color MODE     auto (default) | truecolor | 256 | mono
  -a, --ascii          draw with ASCII glyphs instead of block elements
  -r, --ramp STR       ASCII transparency ramp (default " .:;*oO#%@")
  -i, --interactive    fly with the arrow keys; -w sets speed
  -O, --once           render a single frame to stdout
  -L, --loop SEC       prerender n seconds of frames to loop
  -o, --output FILE    with -L: save prerender to file instead of playing
  -f, --file FILE      play a preredered file
```

