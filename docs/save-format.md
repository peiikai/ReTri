# Saves

Echoes of Wisdom's saves as ReTri keeps them. What we know of the layout is what `runtime/hle/load_save.c` checks; the
rest of the game's save data isn't mapped yet.

## Where they are

The `save/` folder in the data folder (`LARECOMP_SAVE_DIR` uses another one for a run). The game sees it as its save
storage; `runtime/services/fs.c` maps it to files.

```
save/save_0/
  save.meta            the file list the file select shows
  slot00/              File 1
  slot01/              File 2
    file00 .. file05   up to six saved data, newest first in the game's list
      main.bin         the save itself
      maps.bin
      thumb.bin        the picture in the data list
```

## What is known

| File | Field | What |
|---|---|---|
| `save.meta` | +0x0 | `"Ms"` |
| | +0x8 (u32) | size |
| `main.bin` | +0x0 | `"S4"`, then u16 1 |
| | +0x8 (u32) | the file's size |
| | +0xC (u32) | CRC-32 (zlib's) of bytes 0x10 to the end |
| | +0x28 | the save time (Unix seconds) |

All integers are little-endian. `maps.bin` and `thumb.bin` aren't mapped.

## How ReTri writes them

A save is all or nothing, as on the Switch: a file opened for writing on the save mount works on a copy
(`<file>.pending`), and the game's Commit syncs it and renames it over the file. A crash or power loss mid-save leaves
the last committed save. A save that can't be written is dropped whole, the game is told it worked (its SDK would
abort otherwise), and the player gets one warning. `python3 tools/test_save.py` covers this without game files.

## Loading a save directly

`--load-save` (or `LARECOMP_LOAD_SAVE`) boots straight into File 1 or 2, a row of its data list, or a save folder from
elsewhere, which is copied to a scratch folder first. See [debug-hooks.md](debug-hooks.md).
