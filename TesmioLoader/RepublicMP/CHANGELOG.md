# Changelog (SovietRepublic-MP — One State & Two States Co-op)

## v1.4.0 (2026-11-02) — Protocol 54
- **Road Upgrade Split Fix:** Fixed road upgrade split handling and planned construction ownership per state in Two States mode.
- **Two-States Research & Vehicle Blueprints:** Independent university/technical/HQ research trees and vehicle blueprint licenses per republic.
- **Two-States Construction Offices:** Auto-search isolated to own republic while preserving manual mutual-aid assignment across the border.
- **Vehicle Sale & Rename Fixes:** Fixed 7-argument `H_VehicleSale` stack parameter preservation and added full Lockstep support for all 5 `RenameWindow` kinds + city creation.

## v1.3.1 – v1.3.9 (2026-10-30 – 2026-11-02) — Protocols 50–54
- **Per-Player 3D Camera Persistence:** Host and Client 3D camera positions and angles are saved in `republicmp_coop.ini` and restored on load/resync.
- **Smooth 60–144+ FPS Sub-Tick Interpolation:** Road vehicles, trains, coupled wagons, wheels, cabin/follow cameras, and 3D selection outlines interpolated smoothly on held frames.
- **Remote Sound & Notification Isolation:** Partner's 2D UI clicks and foreign territory warnings are muted; 3D construction/demolition audio is spatialized within 250 m.
- **Two-States Economy & Civic Isolation:** Full per-state separation of RUB/USD treasuries, loans, warehouses, auto-purchase sliders, manual resource purchases, customs trade, and citizen demographics.

## v1.0.0 – v1.3.0 (2026-10-19 – 2026-10-29) — Protocols 37–50
- Initial release and expansion of the **Two States on One Map (Western SSR & Eastern SSR)** mode, **Native Gosplan USSR In-Game Menu & Sidebar (`☭ -> Co-op`)**, **1-Click Steam P2P Networking**, **Host-Authoritative Steam Workshop Mod Auto-Sync**, **Isolated Co-op Save Bank (`save_coop`)**, and **`dinput8.dll` Steam Auto-Loader**.
