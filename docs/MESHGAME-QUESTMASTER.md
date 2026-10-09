# MeshGame quest state

## Product direction

MeshGame is a single-player exploration layer inside the LilyGO T-Deck firmware. It reuses the map, GPS, local pins, and Meshtastic node data. The regular Maps app is a separate mode and should remain unaffected. The player should be able to choose real targets, travel to them, inspect or document them, and see persistent progress.

The design source of truth is `docs/MESHGAME-QUESTLOG-PLAN.md` in this repository. It describes candidate sources, evidence states, progression, and later feature families. Its long-term goals must not be described as shipped features.

## Current firmware mechanics

- Local pins are stored in `/pins.csv` on the SD card. Existing pins are backward-compatible; optional `#symbol|…` and `#mission|…` lines hold local game metadata. Meshtastic waypoint sharing remains its own unchanged behavior.
- Available marker symbols are Feather, Wi-Fi, Place, Mission, Loot, Danger, Water, Camp, and Mesh Node.
- A pin becomes a mission when enabled in Game's pin manager. The default GPS arrival radius is 30 m; it can be cycled through 50, 100, and 250 m.
- With a GPS fix, entering the radius marks a mission discovered. The player then confirms completion in the quest log. Discovery and completion each contribute 25 XP in the current code.
- Level thresholds are derived from mission flags: Level 1 starts at 0 XP; each next level costs `100 + 50 × (level − 1)` XP. Current code derives the total from pins rather than a separate profile database, so deleting/reclassifying a mission can change the displayed XP. A robust event ledger and anti-farming system remain future work.
- The map menu has a GPS action: enable/request a fix if needed and center the map on the next valid fix. GPS continues updating independently; the Me button recenters on the latest position.
- Quest rows show coordinates and, with a fix, distance and cardinal bearing. This is a bearing to the pin, not a routable pedestrian path.

## Mission templates in the current source change

The Questlog's `Neu +` flow lets the player choose one of eight templates, then tap a real target position on the map. Existing local pins can use the same template picker. New missions use the corresponding existing marker symbol and store title/hint/status with the pin:

| Family | Symbol | On-site objective |
|---|---|---|
| Exploration | Place | Reach the point and inspect the area |
| WLAN trace | Wi-Fi | Scan nearby networks and note a visible network |
| Mesh trace | Mesh Node | Check actual mesh reception/node presence and record the observation |
| Find / field discovery | Loot | Document a real observation or find |
| Water | Water | Check the water point and its condition |
| Camp | Camp | Assess the location and access |
| Hazard report | Danger | Recheck and update the warning |
| Field note | Feather | Record an observation at the target |

These templates are mission prompts anchored by the player; they do not create fake places. The WLAN scan and Mesh evidence checks are not yet coupled to the quest-completion gate. Current completion is GPS discovery plus deliberate manual confirmation. Do not call this automatic WLAN verification.

The HUD change adds a small lower-left `LV N` plus `XP/next threshold` readout and progress bar, derived from the same saved mission flags as the Questlog. The gameplay/HUD patch compiled successfully for `t-deck-tft` on 9 October 2026. It was flashed application-only at `0x10000` on 9 October 2026; the recorded image hash was verified with esptool. The new UI has not yet been visually checked on the physical display.

## Future extensible families

Consider these only when their evidence source exists:

- WLAN candidate verification from a real OSM/import/manual candidate, then an on-site scan and explicit confirmation.
- Mesh node visit or reception log from real current node/position events.
- Visit an existing user pin; water/camp/landmark check using real places.
- New mapped-cell exploration during a manually started expedition.
- Multi-stop route assignments from selected real pins, journal entries, and day exports.
- Later chapters, achievements, skill points, and player profile, once persistence is separate and duplicate XP is prevented.

No complete public Wi-Fi database exists. The current firmware now embeds a Berlin-only candidate catalogue generated from the project's 2026-09-30 OSM PBF: 1,544 explicit WLAN-tagged objects and 179 U-Bahn stations as BVG WLAN-check targets (21 also carry explicit WLAN tags). The catalogue refreshes when MeshGame opens and when `Quests > Ziele` opens, using GPS or the map centre; it shows up to 12 nearest WLAN plus 12 nearest BVG candidates within 20 km. These are unverified candidates. BVG says its 175 U-Bahn stations have free WLAN, but reception can be limited; OSM records may be stale and usually lack SSIDs. The current firmware does not perform a live WLAN scan; arrival plus manual confirmation records a visit, not verified Wi-Fi. Candidate targets stay temporary until accepted as Game-only pins; they do not alter regular Maps pins or Meshtastic waypoints. The source generator is `tools/generate_meshgame_candidate_catalog.py`.

## UX and engineering invariants

- Never create a mission at arbitrary generated coordinates and imply there is a real point of interest there. For place-based missions, let the player choose the map coordinate or use a verified candidate.
- Label OSM/import data as unverified until visited. Keep source and observation distinct.
- A map-tap mission should retain the selected target coordinates, title, hint, symbol, radius, and status after restart.
- A failed SD save must not leave a visible success or XP award.
- Existing pins without a symbol remain Feather; preserve the current `/pins.csv` parser and serialize optional fields as compatible comment records.
- Only the Game-specific screen/menu/HUD should receive the game presentation. Do not change the regular Maps pin controls, attribution/license obligations, or Meshtastic waypoint payloads.
- For scan-based objectives, preserve Wi-Fi connection state and do not claim SSID/BSSID evidence until a real scan result is captured and saved.


## Preinstalled candidate catalogue – 9 October 2026

The Berlin OSM/BVG candidate catalogue is now compiled into the flashed firmware. To use it: open MeshGame, press `Quests`, then `Ziele`; choose `Karte` to center the map or `+ Quest` to save the candidate as a local 30 m-radius Game mission. At this stage users must inspect the place and manually confirm completion; no actual Wi-Fi scan evidence is captured. Do not describe candidate visibility as proof of a live AP. A scrollable `?` help is present in the Questlog. It explains targets, `Neu +`, Pins, GPS/radius, XP, and current limits. The Game `Label AN/AUS` toggle also hides/shows temporary candidate labels while leaving icons visible. Build hash: `77d4c02c39075a188dbf7dc476b4a7806bb46c59a58da90e3fff5ca5237714d2`; flashed app-only at `0x10000` on 2026-10-09. Display not yet visually checked.
