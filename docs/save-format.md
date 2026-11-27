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

After the header, `main.bin` is a list of blocks: a two-letter tag, u16 version, u32 hash, u32 size (the 16-byte
block header included), in this order: `Ba`, `Lc`, `Pl`, `Cp`, `Fg`, `[]`, `Fl`, `Qt`, `Sr`, `Ui`, `Dc`, `Ka`, `Md`,
`Hr`, `Mn`, `Rp`, `Am`. A 1.0.x file is 0x9d70 bytes; 1.1.0 writes 0xad80, its `Ui` block (version 2) being 0x3070
instead of 0x2060, so the blocks after it move. `Pl` (hash 0x597a2cab) is at +0x160 in both and holds the player:

| Block offset (after its header) | What |
|---|---|
| +0x0 (u32) | rupees |
| +0x4 (u16) | health, in quarter hearts |
| +0x6 (u16) | Heart Containers, one bit each |
| +0x8 (u64) | Pieces of Heart, one bit each |
| +0x10 (u32) | flags; 0x2000, 0x4000, 0x8000: the energy gauge's upgrades |
| +0x74 (float) | Swordfighter Form energy |

Max health isn't saved: the game works it out as 3 hearts, plus one for each Heart Container and each four Pieces of
Heart, at most 20 (in quarter hearts: 12 + 4 x (containers + pieces / 4), at most 80). The energy gauge holds 15, or
20, 30 or 40 with its upgrades.

### In memory

While a save is in play the game keeps three images of `main.bin` in one object, (size + 0x420) bytes apart: the
save as loaded, the game's current data, and an empty one. The middle one is live: the game reads it (the max health
above comes from its `Pl` block) and saves from it, with a fresh checksum. Health, max health, rupees and the energy
gauge are played from a status object of their own instead (`runtime/hle/cheats_other.c`) and copied into `Pl` when
the game saves. The Cheats page changes the status; the Save Editor (`runtime/hle/save_other.c`,
`runtime/ui/save_editor.mm`) changes both, and the game's next save writes them.

## How ReTri writes them

A save is all or nothing, as on the Switch: a file opened for writing on the save mount works on a copy
(`<file>.pending`), and the game's Commit syncs it and renames it over the file. A crash or power loss mid-save leaves
the last committed save. A save that can't be written is dropped whole, the game is told it worked (its SDK would
abort otherwise), and the player gets one warning. `python3 tools/test_save.py` covers this without game files.

## Loading a save directly

`--load-save` (or `LARECOMP_LOAD_SAVE`) boots straight into File 1 or 2, a row of its data list, or a save folder from
elsewhere, which is copied to a scratch folder first. See [debug-hooks.md](debug-hooks.md).
