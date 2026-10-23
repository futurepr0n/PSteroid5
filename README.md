# PSteroids5

Native PS5 port of [CSSteroids-2](../CSSTeroids-2), built as its own project.
CSSTeroids-2 is a **read-only reference** for gameplay rules and the server API;
this port talks to that project's unchanged Node/MySQL server over the network
and makes no changes to its source.

- Port details, controls and server setup: [ps5/README.md](ps5/README.md)
- Plan and milestones: [docs/plans/2026-10-23-ps5-native-port.md](docs/plans/2026-10-23-ps5-native-port.md)
- Workspace rules: [../AGENTS.md](../AGENTS.md), [../PS5-PORTING-HANDBOOK.md](../PS5-PORTING-HANDBOOK.md)

Build (Windows PowerShell): `& .\Build-PS5.ps1 *> .\build-ps5.log` → `dist/PPSA99305/`.
