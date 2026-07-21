# cc-generals-overlay — Agent Context (main branch)

> 📌 **Lokaal bestand** — niet committen naar GitHub.

## Branch: `main`
**Doel:** Spectator overlay voor C&C Generals Zero Hour — in-game unit queues, power cooldowns, click-to-navigate.
**Repo:** `https://github.com/nathan-soul/GameClient` (fork)
**Lokaal pad:** `~/projecten/cc-generals-overlay/`

## Wat deze branch doet
- **Unit queues** per speler: War Factory, Barracks, Airfield (max 5 entries, groene voortgangsbalk)
- **General power cooldowns**: linker/rechter schermrand, cooldown balk, groene flits bij gebruik
- **Click-to-navigate**: klik op power icoon → camera naar target
- **SEH-safe**: alle productie-queue access wrapped in `__try/__except` voor replay mode (linked list corrupt)
- **Font scaling**: Shift+Up/Down past queue icoon grootte aan

## Key files
| File | Functie |
|------|---------|
| `GeneralsMD/Code/GameEngine/Source/GameClient/InGameUI.cpp` | Alle overlay rendering + safe wrappers |
| `GeneralsMD/Code/GameEngine/Include/GameClient/InGameUI.h` | Overlay structs (PlayerOverlayExt, QueueEntry, etc.) |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/Object/SpecialPower/SpecialPowerModule.cpp` | Hook in `triggerSpecialPower()` |

## Build
```bash
cd ~/projecten/cc-generals-overlay
bash scripts/docker-build-msvc.sh
# Output: build/msvc-wine/.../GeneralsOnlineZH.exe
```

## Gerelateerde projecten
- **cc-replay-scraper-engine** (`~/projecten/cc-replay-scraper-engine/`, branch `replay-scraper`): zelfde codebase, andere branch — headless replay processing met StatsExporter uitbreidingen
- **cc-replay-scraper** (`~/projecten/cc-replay-scraper/`): Docker workers + web viewer voor replay data

## Afspraken
- Dennis werkt aan overlay features op `main`
- Releases via `nathan-soul/GameClient` fork, niet upstream PR
- Build output als `.zip` met `GeneralsOnlineZH.exe` (niet zippen, direct .exe erin)
- Test via replays in observer mode
