# About this program

This program for monitoryng system info.

Program name `stat`

Should support `linux` and `windows`.

## Dirstat. First functional i want to add research disk space, which folder, file takes up space

Alternative program is https://windirstat.net

Program should support cli and api

Server mode
```bash
state -s 127.0.0.1:8084
```

Example usage.
 
```bash
# reasearch all system space, 
# after that, should cache result
stat dirstat -s (--scan) /

# show like three from cached result
## should visible when was scanned this path
stat dirstat -t (--three) /

```

On api dirstast, should return result, which will used show Squares on html, like on windirstat.


---

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/stat --help
```

> The binary is called `stat`, the same as the coreutils `stat` command. Run it as
> `./build/stat`, or put it on your `PATH` under another name, so scripts that use
> `/usr/bin/stat` keep working.

## CLI

```bash
stat                             # system overview: CPU, memory, swap, disks/free space, GPU
stat --json                      # the same as JSON (same format as /api/system)
stat dirstat --scan /            # scan, cache the result, print a summary
stat dirstat --tree /usr -d 3    # tree from the cache (also: -t, --three)
stat dirstat --list              # cached scans and when they were made
stat dirstat --tree /usr --json  # same JSON as the API
```

| option | meaning |
|---|---|
| `-d, --depth N` | tree levels to show (default 2, 0 = unlimited) |
| `-n, --top N` | biggest entries per directory (default 10, 0 = all) |
| `-x, --one-file-system` | do not cross into other filesystems while scanning |
| `-j, --json` | JSON output |
| `-q, --quiet` | no progress/summary |

`--tree` works for any path inside a cached scan. If you scanned `/`, then
`stat dirstat -t /home` reads from that scan, and the output shows when the scan ran.
If several scans contain the path, the newest one is used.

Sizes are bytes on disk (like `du`). Hard links are counted once.
Virtual filesystems (`/proc`, `/sys`, cgroup, …) are skipped.

Cache: `$STAT_CACHE_DIR`, else `$XDG_CACHE_HOME/stat/dirstat`, else `~/.cache/stat/dirstat`.

## Server / API

```bash
stat -s 127.0.0.1:8084
```

| page | what it shows |
|---|---|
| http://127.0.0.1:8084/ | live overview: CPU (total + per core, 2‑minute history), memory, disks and free space, GPU |
| http://127.0.0.1:8084/dirstat | disk-usage treemap (squares like WinDirStat; click to zoom in) |

The "Analyze →" link next to each disk on the overview page opens that mount point in dirstat.

| method | endpoint | description |
|---|---|---|
| GET | `/api/system` | snapshot: cpu, memory, disks, gpus (CPU sampled every 1 s, GPU every 2 s) |
| GET | `/api/dirstat?path=/usr&depth=3&limit=1000&min=0.0005` | tree from cache |
| POST | `/api/dirstat/scan?path=/` | start a background scan (`202`, or `409` if one is running) |
| GET | `/api/dirstat/status` | progress of the current/last scan |
| GET | `/api/dirstat/scans` | list cached scans |

`/api/dirstat` response:

```json
{
  "root": "/", "path": "/usr", "scanned_at": 1793421743, "duration_ms": 2628, "errors": 4,
  "tree": {
    "name": "/usr", "type": "dir", "size": 1602277376, "items": 34124,
    "children": [
      { "name": "lib", "type": "dir", "size": 786501632, "items": 6821, "truncated": true },
      { "name": "7 more items", "type": "rest", "size": 350027776, "items": 4629, "count": 7 }
    ]
  }
}
```

- `type`: `dir`, `file`, `other`, or `rest` (smaller siblings grouped into one entry)
- `truncated`: the directory has children below the requested `depth`
GPU data: NVIDIA comes from `nvidia-smi` (it must be installed), AMD from
`/sys/class/drm` (usage, VRAM, temperature). Intel only reports clock speed, because
its driver does not expose a usage percentage. Values a driver doesn't report are `null`.

- `min`: siblings smaller than this share of the requested node are grouped into `rest`, which keeps responses small enough to draw
