# cutedash

Lightweight terminal system dashboard written in C.

## Stack
- C + ncurses
- Reads from /proc and /sys directly (no external deps)

## Build & Install
```
make
sudo make install    # installs to /usr/local/bin/cutedash + stats symlink
```

## Run
```
stats           # or cutedash
```
Press `q` to exit.

## Panels
- CPU: per-core bars, average with sparkline history, load averages
- Memory: RAM usage, cached/buffers, swap
- Temps: hwmon sensors with bars
- Processes: top by memory usage
- Network: live up/down speed with sparklines
- Disk: mount usage bars with sizes
