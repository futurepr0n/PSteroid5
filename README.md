# PSteroid5

A native PS5 homebrew port of [CSSteroids-2](https://github.com/futurepr0n/CSSteroids-2), the browser
Asteroids game. It plays against the same live server as the browser version, so PS5 and browser
pilots share high scores, ships and the online world.

- Single player arcade action
- MULTIPLAYER: two-player co-op rooms over a 10-round campaign
- MMO: drop into the shared online world with browser players at any time
- Power-ups in every mode: extra life, rapid fire, spray and shield
- Hangar: fly any ship from the public gallery, or load your own by passphrase
- Ship editor: build and save custom ships with the controller (touchpad drag supported)
- Online high scores

Tested on PS5 Phat/BD, firmware 4.03.

## Install
Extract the release zip and copy the `PPSA77305` folder to `/data/homebrew/` (or `homebrew/` on a
USB drive), then scan with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus).
Title ID `PPSA77305`. If you installed an earlier build as `PPSA99305`, delete that tile first.

## Source
CSSteroids-2 is a **read-only reference** for gameplay rules and the server API; this port talks
to that project's unchanged Node/MySQL server over the network and makes no changes to its source.

- Port details, controls and server setup: [ps5/README.md](ps5/README.md)
- Plan and milestones: [docs/plans/2026-10-23-ps5-native-port.md](docs/plans/2026-10-23-ps5-native-port.md)
- Ship editor evidence: [docs/ship-editor-report.md](docs/ship-editor-report.md)

Build (Windows PowerShell with the PS5 PayloadSDK/Vulkan toolchain in WSL):
`& .\Build-PS5.ps1 *> .\build-ps5.log` → `dist/PPSA77305/`.

## License
GPL-3.0-or-later (see [LICENSE](LICENSE)). Bundled components keep their own licences:
mbedTLS (Apache-2.0, `third_party/mbedtls`), and the PS5 platform, renderer and toolchain
libraries listed in the package's `THIRD_PARTY_NOTICES.txt` and `licenses/`.
