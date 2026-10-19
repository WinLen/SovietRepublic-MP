# Changelog

## 0.5.4

Desync fixes for building and terraforming in lockstep. Protocol 35 - both
players need this build.

- **Forklift paths** - factory connections attach to buildings again in
  lockstep; the buildings' connection points were missing.
- **Placement** - a building's access road or path is laid the same way on
  both PCs, whatever the cameras' angles and whatever each player's mouse
  points at (bus stops, substations and garages next to roads split the
  games); the building tool no longer sees what one camera sees while a
  placement is made.
- **Construction sites** - a site under one player's camera no longer runs
  differently on that PC; sites under construction no longer flicker while
  more of the same building is placed.
- **Terraforming** - a paid terrain stroke's entry in the construction
  ledger is shared, so the construction sites pay the same on both PCs
  after terraforming.
- **Fixes** - opening a building window in a session no longer crashes the
  game.
- **Debug** - a money trace (off by default): on a desync each PC logs its
  balances and construction sites of the ticks before it; a placement's log
  line lists the objects it made.

## 0.5.3

Lockstep that no longer depends on the camera, a reworked window and a DLC
check. Protocol 28 - both players need this build.

- **Camera-free simulation** - for the simulation every building is off the
  camera on every PC, so moving the cameras apart no longer splits the games
  (tested for 54,000 ticks with the cameras far apart); every citizen is
  updated every tick, so citizens walk smoothly; each PC draws the citizens,
  stops included, and building doors near its own camera (by SoylentGreed).
- **Fewer desyncs** - the game's K debug key no longer changes the
  simulation on one PC; city and crossroad renames are shared (by
  SoylentGreed).
- **Hyper and Plaid** - Ctrl+Numpad1 / Ctrl+Numpad2 (or the fast button held)
  work in lockstep; the host's, the player's keys are requests.
- **DLC check** - a join with different DLCs is refused while both players
  keep "Check DLCs" ticked; either can untick it to play anyway (#30).
- **Window** - text wraps; the mode is chosen as Lockstep or Replay; a
  lockstep panel shows the simulation, the ping, the tick flow and a desync
  on both PCs, with a re-sync a set time after a desync (1 minute by
  default); replay-only indicators stay in replay mode.
- **Fixes** - a session whose load refused the patches no longer leaves the
  host paused; the F8 / F9 / F10 dev hotkeys and the heartbeat log are gone
  (the terrain log is in the Debug window).

## 0.5.2

More of the game shared in lockstep. Protocol 23 - both players need this
build.

- **Roads and junctions** - road signs, junction rules (right-hand, main
  road, traffic lights) from the junction window and the decoration tool,
  and rail signals (by SoylentGreed).
- **Game rules** - changes on the settings page apply on both PCs; a
  critical rule no longer saves and reloads one game only.
- **Vehicles** - selling, per-stop cargo settings, composing trains in the
  depot (couple, uncouple, move wagons) (by SoylentGreed).
- **Buildings** - cargo filters of links and specific sources (the add-source
  tool, the source windows) (by SoylentGreed).
- **Fixes** - the vehicle window's "Carried" list shows the cargo again;
  traffic lights and building doors no longer blink; the camera no longer
  flies off.

## 0.5.1

More of the game shared in lockstep. Protocol 22 - both players need this
build.

- **Terrain brushes** - raise, lower, the three levelling brushes, tree
  demolition and topsoil restoration run as the game's own tool on both PCs
  with the clicking PC's inputs (cursor, brush, frame time, level height),
  so vegetation, topsoil and costs come out the same (by SoylentGreed).
- **Trees** - the single-tree tools plant the same trees on both PCs (#27,
  by SoylentGreed).
- **Decoration paths** - the second-road paths and pavements (airplane,
  basic road, l2-l4) are drag commands like roads (by SoylentGreed).
- **Window settings** - what a click changes in a building's window, its
  storage slots (auto purchase amounts, "store this item") or the bottom
  menu's world settings is put back and carried; the bottom menu's
  every-frame rewrite no longer splits the world settings.
- **Farms and offices** - adding a field to a farm, and unassigning fields
  and buildings from farms and offices, removing links (by SoylentGreed).

Still put away in lockstep: removing decoration paths, worker relocation,
buoys, Christmas lights, junction and merge tools, "Go there".

## 0.5.0

Lockstep: both games run the same simulation tick by tick. This is a rewrite
of how the two games stay together - vehicles, citizens and production no
longer drift apart. Protocol 21 - both players need this build. Replay mode
stays available.

- **Lockstep sessions** - **Host in lockstep** makes the next session run in
  lockstep; the joining player follows the host. The host saves, both PCs
  load the same save in parallel, and from there the host admits the ticks
  (at most 10 ahead of the player) and orders the commands. Pause and speed
  are the host's. A state hash is compared every 60 ticks; the first
  difference is shown in the RepublicMP window and can trigger a re-sync. A
  keepalive ends the session after 20 s of silence.
- **Determinism layer** - fixed time step, random numbers per call site,
  pinned math, zeroed allocations; the camera, the windows and the tools are
  kept out of the simulation (private view and search lists for the tools,
  window cost displays, per-frame window work). The Debug window's probe runs
  a save twice, with held frames, speed changes and recorded building, and
  compares every tick (by SoylentGreed).
- **Construction as commands** - placement (taken at the game's own decision,
  with levelling and access roads), every drag tool, upgrades, planned
  construction, group tools, terrain edits, building demolition, and the
  bulldozer: one command per piece, removal drags split into several
  commands, pieces removed through a hovered node carried with that node.
  Edges a command creates go back into the network's visible-edge lists, so
  the pieces a cut leaves can be picked at once.
- **Purchases** - vehicles (new and used), licences and factory blueprints
  bought in the purchase windows (by SoylentGreed).
- **Lines and routes** - adding stops, putting vehicles on a line, the depot
  pick tool, creating and deleting lines, the line window's Accept (by
  SoylentGreed).
- **Windows** - the vehicle and line windows' buttons as commands; vehicle
  settings changed in a window are put back and carried; the vehicle
  window's per-frame route rebuild no longer runs in a session; building
  window settings, storage slots and construction pay / stop; the farm field
  tool (by SoylentGreed).
- **Logs** - every command logs the money it changed, so a money difference
  can be traced to the command.

Known issues:
- The construction animation (scaffolding, ground, workers) is not drawn in
  lockstep.
- Not carried yet: trees, worker relocation, pavements, buoys, Christmas
  lights, junction and merge tools, "Go there", goods purchases and the auto
  purchase window, research, world settings.
- Some house window settings may still differ between the PCs.

## 0.4.3 (pre-release)

Planned construction, the group tools and power lines now come out the same
on both PCs; hosting needs a loaded city. Protocol 18 - both players need
this build.

- **Starting planned roads and rails** ("start construction" in the
  construction menu, the construction window or the network tool panel)
  now turns the same plans into construction sites on the other PC too;
  before, they stayed white plans there (#20). The new sites take the
  sender's construction mode (rubles, dollars or realistic, auto purchase),
  not the receiver's. Protocol 18.
- **Group construction tools** (the drag box that starts plans for rubles,
  dollars or resources, and the one that suspends construction) now act on
  the other PC too, with the same box (by SoylentGreed).
- **Power lines joined to an existing line end**: on the other PC the new
  line's end was lifted by the pole height (20 m for high voltage) and became
  a separate node above the old one, so the line carried no power there and
  the sender's demolition later missed that piece. The replay now joins the
  same node (#22, #23, by TucKnock).
- **Demolishing the middle of a power line** could move the line's own end
  node (e.g. at a switch's port) onto the cut on the other PC, so the next
  demolitions removed the wrong spans and a couple stayed there. The cut now
  takes a fresh node, as the game's own tool does, and a power line is cut
  exactly where the sender cut it - not one span further (#23).
- **Hosting needs a loaded city**: Host is unavailable in the main menu.
  Leaving the world (back to the main menu or loading another save) ends the
  session - stops hosting or disconnects - instead of leaving the other
  player connected to a game that is gone.
- **Open windows** no longer hold the other player's buildings and roads:
  a replay runs with the menus closed for its call and puts them back
  after (#21, by TucKnock).

## 0.4.2

The other player's camera no longer decides how a building or a road comes
out.

- **Access roads**: a building's automatic access road is laid with the same
  collision checks and the same nearby roads on both PCs, instead of what
  each player's camera sees. Before, it could come out as a road on one PC
  and a footpath on the other, or take another route, so the sender's later
  demolition missed it and new buildings were refused there (by
  SoylentGreed).
- **Demolishing roads** works wherever the other player is looking (the pick
  and the piece lookup no longer use the camera's list of roads). A piece
  the game has not settled yet after a neighbouring demolition is tried
  again a moment later, and a piece already gone counts as done. A
  building's access road laid by the other PC's own game is now demolished
  too, and a piece whose building is still being demolished there waits
  for it.
- **Footpaths joined to a road** get their pedestrian crossing on the other
  PC too: where the path splits the road (or meets its node) now travels
  with the path. Protocol 17.
- **Alt-tab**: a game in the background (low frame rate) no longer builds
  differently from the other player's; construction sites a camera sees no
  longer get faster worker updates; a new citizen's value is set by the
  simulation only, never first by a window or overlay on one PC. All of
  these are active only while connected (and in the determinism probe) (by
  SoylentGreed).
- **Construction sites**: stop / continue / money finish is sent only from
  that building's own window, never the game's own clean-up of a finished
  site (#17) or a new site drawn with the road tool.
- **Placement**: the replayed building is found by identity and checked
  against the sender's position before it gets the sender's rotation (#16);
  an access road is no longer built twice, or for a building that could not
  be placed.
- **Joining**: both games stay paused after a successful join or re-sync
  until either player resumes (#18).
- **Citizens' destinations**: where a home's or bus stop's workers,
  students, passengers and tourists go (up to 9 buildings per list) is
  synced, including removing them (#15, by TucKnock).

Known issues:
- Realistic construction can still drift apart (e.g. a road at different
  percentages) - each PC simulates its own workers and vehicles.
- Buying vehicles and a shop's or pub's goods purchases are not synced yet.
- While a player has a window open, buildings and roads from the other
  player wait until it is closed.

## 0.4.1

Fixes and the construction-site controls.

- **Construction sites**: stopping and continuing construction, and
  cancelling "finish with rubles / dollars", now reach the other player
  (before, only switching the money finish on did). They apply at once even
  while the other player has a window open.
- **Moving-in restrictions**: all eight "who may move in" flags of a house
  are synced (before, only "nobody").
- **Roads**: a road message carries only the points actually used - a short
  path is ~200 bytes instead of ~48 KB. Paths can have up to 2048 points.
- Fixed: a crash on the other side when a footpath was replayed while that
  player's cursor was over a road.
- Drift check: per-subsystem floors (2 vehicles against 0 is no longer
  100%), vehicles count fully, the limit must be exceeded 3 game hours in a
  row and an automatic re-sync waits at least 10 minutes after the last one;
  summaries that arrive early are no longer dropped.
- Weather: only the host's rain decision is taken over; dawn, dusk and night
  follow the local clock (no more flickering light).
- The build mode in the log reads "rubles, auto purchase on 70% option 1,
  fill 20" instead of raw numbers.
- Debug window: "Log building fields changed by clicks (FIELD_DIFF)", a
  helper for finding window settings that are not synced yet.

Known issues:
- Stopping or paying for a road / rail construction site may not reach the
  other player (their site is not found at the same spot); a re-sync brings
  it over.
- While a player has a window open, buildings and roads from the other
  player wait until it is closed.
- Buying vehicles is not synced yet; a re-sync brings them over.

## 0.4.0

The world itself starts to be synced, not only what players build.

- **Weather sync** - the host owns temperature, wind, snowfall and rain; the
  player's game takes them over every second (#13).
- **Drift check** - every game hour both games compare citizens, vehicles,
  storage and production; the host window shows the drift and can re-sync
  automatically above a chosen limit (#14).
- **Determinism layer and probe** - groundwork for simulating both games in
  lockstep: fixed time step, call-site random numbers and pinned math,
  inactive in normal play. The Debug window's probe runs the same save twice
  and reports whether both runs stay identical (#12).
- Fixed: two build errors left by merging #13 and #14.

Known issues:
- With very few vehicles or citizens the drift percentage jumps (2 vehicles
  against 0 counts as 100%), so "Re-sync on drift" can trigger needlessly -
  keep it off or at 20% on a small city.
- Buying vehicles is not synced yet; a re-sync brings them over.
- A footpath drawn from a building port can fail on the other side; a
  re-sync brings it over.

## 0.3.9

- **Money sync** - the host's balances apply to both players (#8).
- **Re-sync** - the host can send its world to the player again, by hand or
  every 5-60 minutes; the player reconnects by itself (#9).
- **Building settings** - what players set in a building's window is synced
  (#11).
- **Factory connections** are synced, including their direction (#10).
- Roads and other paths can have up to 2048 points (was 512).
- Fixed: a remote building was not placed when the receiving player had no
  construction tool selected (#6, issue #3), and some buildings (grain silo,
  cement plant, ...) still failed in debug builds.
- Fixed: crash at startup with the Steam overlay (#5, issue #2).
- Fixed: a debug-build assertion when several buildings were placed quickly
  and a terrain batch ended with a height edit.
- The received save is refused if it contains unsafe file names (#4).
- When the live sync channels fail to connect, the error names the ports to
  forward (#7).

Thanks to veryinsanee for the pull requests and TucKnock for the detailed
bug reports.
