# RepublicMP

Multiplayer mod for *Workers & Resources: Soviet Republic*, built as a
[TesmioLoader](https://github.com/MaxLegend/TesmioLoader) plugin. Two players
share one city: the host's game is transferred to the joining player, and
from there both games run the same simulation tick by tick (**lockstep**),
or each game simulates on its own and the players' actions are repeated on
the other side (**replay mode**).

**Status:** alpha, tested with two players. Current version: **0.5.4**
([changelog](CHANGELOG.md)).

## Support and community

Questions, bug reports and test partners are welcome on
[Discord](https://discord.gg/BRTAYNKAYu). If you want to support the work,
you can do it on [Buy Me a Coffee](https://buymeacoffee.com/3418).

## Two modes

| | Lockstep (recommended) | Replay mode |
| --- | --- | --- |
| How | Both games run the same ticks; every player action is a command both games make after the same tick | Each game simulates on its own; actions are sent and repeated on the other side |
| Vehicles, citizens, production | The same on both PCs | Drift apart; a re-sync brings them back |
| Speed and pause | The host's (Hyper and Plaid included); the player's keys are requests | The host's; the player's keys are requests |
| Checks | Both games compare a state hash every 60 ticks; a difference is shown on both PCs and can re-sync after a set time | A drift check every game hour |

The host picks **Lockstep** or **Replay** in the RepublicMP window before
pressing **Host**; the joining player follows the host's choice.

## Lockstep

A determinism layer makes the game simulate the same way on every PC: a fixed
time step, random numbers per call site, pinned math, zeroed allocations, and
the camera and the windows kept out of the simulation - for the simulation
every building is off the camera on every PC, while each PC still draws what
its own camera sees. The host admits the ticks (at most 10 ahead of the
player) and orders the players' commands. The lockstep panel shows the tick,
the ping, how the ticks flow and whether the games match.

Carried as commands:

- **Construction** - building placement (with terrain levelling), every drag
  tool (roads, rails, footpaths, pipes, power lines, fences, conveyors,
  cableways, factory connections, decoration paths), upgrades, planned
  construction, the group tools, building and network demolition with the
  bulldozer.
- **Terrain and trees** - the terrain brushes (raise, lower, levelling, tree
  demolition, topsoil restoration) and the single-tree tools.
- **Building windows** - settings and storage slots (auto purchase amounts),
  the bottom menu's world settings, pay with rubles / dollars and stop on
  construction sites, factory connection directions, adding and removing
  fields of a farm, office assignments and links, the cargo filters of links,
  specific sources (the add-source tool and the source windows).
- **Roads and junctions** - road signs, junction rules (right-hand, main road,
  traffic lights) from the junction window and the decoration tool, rail
  signals.
- **Game rules** - changes on the settings page apply on both PCs.
- **Names** - renaming buildings, vehicles, lines, cities and crossroads.
- **Vehicles** - purchases (new and used vehicles, licences, factory
  blueprints), the vehicle window's buttons (send to depot, release, stop /
  start, clear line, next skin), vehicle settings changed in its window
  (checkboxes, limits, per-stop cargo settings, cargo limits), sending to a
  depot with the pick tool, composing trains in the depot, selling vehicles.
- **Lines and routes** - creating and deleting lines, the line window's
  Accept, adding stops, putting vehicles on a line, launching vehicles.

Not carried yet (tools are put away when selected, or the action acts on one
PC only): removing decoration paths, worker relocation, buoys, Christmas
lights, merge tools, "Go there", goods purchases in shops, research. Saving
is refused inside a lockstep session. The construction animation
(scaffolding, ground, workers on a building site) is not drawn; the building
still finishes on time. While the game is paused, what comes into view
appears only after the pause.

## Replay mode

1. **Buildings and infrastructure** - placed, upgraded and demolished the
   same way on both PCs, with the sender's construction mode.
2. **Terrain** - every heightmap and terrain-mask edit is replayed.
3. **Building settings** and **citizens' destinations** travel to the other
   side.
4. **Speed, clock, money and weather** are the host's.
5. **Drift check and re-sync** - every game hour both games compare
   citizens, vehicles, storage and production; the host can send its world
   to the player again by hand or automatically.

## How it works

The mod hooks the game's own functions instead of simulating input. An
action is taken where the game decides it (a placement click, a drag
commit, a Buy press, a pick tool's choice, a window's routine call), and
every PC makes it again with the game's own code: in lockstep at the same
tick boundary, in replay mode as soon as it arrives. Objects travel as
positions or as their place in the world's lists, never as pointers.

Both players must run the same build of the mod: the version, the protocol
number and a checksum of `RepublicMP.dll` are compared on join, and a
mismatch refuses it.

## Building

1. Clone [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) and clone
   this repository into its root folder (`TesmioLoader\RepublicMP`). The
   project uses `TesmioLoader\src` for headers and writes the DLL to
   `TesmioLoader\build\plugins\`, where TesmioLoader loads plugins from.
2. Optional: copy `mp_config.h.example` to `mp_config.h` and set
   `REPUBLICMP_HOST_IP` to prefill the host address. The file is gitignored;
   without it the address is simply typed in the game.
3. Open `RepublicMP.slnx` in Visual Studio 2026 (toolset v145) and build
   **Release x64**, or from a developer prompt:

   ```
   MSBuild RepublicMP.vcxproj /p:Configuration=Release /p:Platform=x64
   ```

## Usage

1. Both players start the game with TesmioLoader and the same
   `RepublicMP.dll` installed.
2. The host loads a city, picks **Lockstep** or **Replay**, and presses
   **Host** in the RepublicMP window.
3. The other player enters the host's IP (LAN or VPN) and presses **Connect**.
   Both games pause behind the join screen while the city is saved, sent and
   loaded.
4. Once both sides resume, the session runs. **Disconnect** (client) or
   **Stop hosting** (host) ends it.

Both games need the same DLCs: with **Check DLCs** ticked on both PCs (the
default) a join with different DLCs is refused and names the difference;
either player can untick it to play a map that needs no DLC content.

Firewalls must allow TCP ports 5556-5559 on the host, and 5560 for lockstep.

### Controls

| Key | Action |
| --- | --- |
| **Insert** | Show / hide the RepublicMP overlay |

The **Debug** button in the main window opens manual reconnect controls, the
determinism probe, the money trace (both players tick it; on a desync each log
gets the balances and construction sites of the ticks before it) and the
detailed log. A full log is written to
`tesmioloader.log`; when reporting a problem, attach the logs of both
players and say which mode you were in.

## Versioning

`MAJOR.FEATURE.PATCH` - `MAJOR` for a stable milestone, `FEATURE` when a new
capability is added, `PATCH` for fixes within a feature.

`REPUBLICMP_PROTOCOL` in `version.h` is raised whenever a network message
changes.

## Credits

The lockstep work builds on SoylentGreed's research and code (RepublicLAN);
parts taken from it are marked "(by SoylentGreed)". Thanks to veryinsanee
and TucKnock for pull requests and detailed bug reports.

## License

[GPL-3.0-only](LICENSE) for everything after the tag `v0.4.3-mit`. Versions up
to that tag were published under the MIT License and stay under it.
TesmioLoader, which loads this plugin, is GPL-3.0 as well.
