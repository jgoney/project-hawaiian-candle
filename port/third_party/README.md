# Other people's code

Kept exactly as downloaded on 2026-11-13; do not edit these files. To
replace one, swap the files and run `make -C port test`.

| Folder | What | From | Commit | Licence |
|---|---|---|---|---|
| `z80/` | Z80 processor | https://github.com/superzazu/z80 | d64fe10a2274e5e40019b1086bf7d8990cbc5f23 | MIT (`z80/LICENSE`) |
| `nuked-opn2/` | YM2612 / YM3438 FM chip (Nuked-OPN2 1.0.9) | https://github.com/nukeykt/Nuked-OPN2 | 335747d78cb0abbc3b55b004e62dad9763140115 | LGPL 2.1 or later (`nuked-opn2/LICENSE`) |

Neither contains anything from the game. They are built with their own
compiler flags (see `port/Makefile`), not the port's.
