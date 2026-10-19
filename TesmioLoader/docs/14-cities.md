# Cities — a radius you can change, and a square one

**A plugin**, not part of the loader: `plugins/cities/cities.cpp` builds to
`build/plugins/cities.dll`, and deleting that DLL restores the base game
exactly. See [09-plugins.md](09-plugins.md) for the mechanism.

## What a city is, in the engine

A "name point". Nothing more: a name, a position, and the list of buildings that
came out nearest to it.

| Game offset | Contents |
|---|---|
| `+0x10348` … `+0x10350` | `vector<NamePoint*>` — every city on the map |
| `+0x10330` … `+0x10338` | `vector<wstring*>` — the unused-name pool, `city_names.txt` |
| `+0x11B08` … `+0x11B10` | `vector<Building*>` — every building |
| `+0x590` / `+0x594` | the current game date, two `int`s |

**The game object is a static, at rva `0x9D4F10`.** Not a pointer to one: the
save loader reaches the same vector both ways, as `[0x9E5258]` and as
`[gameobj+0x10348]`, and the difference is exactly `0x10348`. The loader spells
it out at `0x43966A`, `lea rcx,[0x9D4F10]` two instructions before it hands a
building's position to the city search.

A `NamePoint` is `0x1C0` bytes, `operator new`'d in one place:

| NamePoint offset | Contents |
|---|---|
| `+0x04` | `float x, y, z` — the city centre |
| `+0x10` | `wchar_t name[128]` |
| `+0x110`, `+0x114` | two `int`s, zero on creation, saved and restored. **Purpose unknown** |
| `+0x118` | `u8` — a *transient* flag, see below |
| `+0x11A` | `wchar_t[64]`, only written when non-empty |
| `+0x1A0` … `+0x1B0` | `vector<Building*>` — the buildings in this city |
| `+0x1B8` | a `0x400` statistics block; its first two `int`s are the date established |

and a building points back at its city:

| Building offset | Contents |
|---|---|
| `+0x108` | `NamePoint*`, or null |
| `+0x318` | the type record; **null on scenery** and on the imported `civ_*` set |
| `+0x320` | the `C3D_NODE`, for `GetPosition` |
| `+0x5D0` | the bounding box: `->+0x20` min, `->+0x28` max, floats at `+4`, `+8`, `+0xC` |

`+0x118` is worth being clear about because it looks like a type field and is
not. `CanDeleteCity` sets it to 1, runs the whole assignment as if the city were
not there, and clears it again; the search skips any point carrying it. It is a
scratch flag with a lifetime of one function call.

## The functions

| RVA | What |
|---|---|
| `0x3305F0` | `NamePoint* FindCity(Game*, const C3DVECTOR3* pos, bool create)` |
| `0x32FEA0` | `void ReassignBuildingsNear(Game*, const C3DVECTOR3* pos)` |
| `0x32F3B0` | `NamePoint* AddNamePoint(Game*, const C3DVECTOR3* pos, const wchar_t* name)` |
| `0x32D1A0` | create a city here, with a generated name — what the editor's "Create city/area" runs |
| `0x32F520` | `bool DeleteCity(Game*, NamePoint*)` |
| `0x32F810` | `bool CanDeleteCity(Game*, NamePoint*)` — the "some buildings will be too far away" test |
| `0x330310` | renumber a city's buildings — the per-type house numbers, into `building+0x110` |
| `0x333440` | `void SaveNamePoints(Game*, FILE*)` |
| `0x3337D0` | `void LoadNamePoints(Game*, FILE*)` |
| `0x7CEDB0` | the city information window |
| `0x1BE610` | the city hall's per-tick, building type `0x27` |

### The one number

`FindCity` is the whole mechanic:

```
limit = [0x90B214]                  ; 1e6  - the radius SQUARED. 1000 m.
best  = [0x90B23C]                  ; 1e10 - "nothing found yet"
for (np : game->namepoints)
    if (np[0x118]) continue
    d = C3DCalcDistanceSq(pos, np+0x04)      ; three dimensions, squared
    if (limit <= d) continue
    if (best  <= d) continue
    best = d; result = np
if (result)  return result
if (!create) return 0
... generate a name nobody is using, AddNamePoint, ReassignBuildingsNear ...
```

One `movss xmm7,[rip+disp32]` at `0x330658`, one literal, every city on the map.
There is nowhere in the `NamePoint` a per-city figure could have been stored,
and nothing else in the game reads a city radius at all.

**A building is placed into a city by distance and by nothing else.** No road
graph, no reachability, no ownership. `ReassignBuildingsNear` pre-filters with a
1000 m *box* around the point that changed — `[0x90B094]`, spelled out again in
`DeleteCity` and `CanDeleteCity` — and then asks `FindCity` about each survivor.

The centre it measures is the node position, except on a building with no type
record, where it is the bounding-box midpoint. That is the game's own rule and
this plugin copies it exactly.

### What the city hall has to do with it

`$TYPE_CITYHALL` is building type `0x27`. Its tick writes the city's own
production figures into `*(city+0x1B8)+0xE0` — the statistics block behind
"Date established". The window dispatcher at `0x6EA0E3` scans the city's
buildings for one and picks the caption from what it finds: `13508` when there
is no city hall, `13507` when there is one that is not working, and when there
*is* a working one it draws that building's whole panel inside the city window
as well. Nothing about the city hall touches the radius.

## What the plugin changes

Four patches and two rewritten calls. Every one is checked byte for byte before
anything is written.

| Site | What | Why |
|---|---|---|
| `0x3305F0` | inline hook | the search, per city |
| `0x330658` | repointed to a `0.0f` this plugin owns | so the original's own search never matches |
| `0x32FEA0` | inline hook | the reassignment, null-safe and no longer 1000 m wide |
| `0x32F57D`, `0x32F862` | repointed to the widest radius | the two delete checks keep their own copy of the box |
| `0x7CEDB0` | inline hook, additive | the slider and the shape checkbox |
| `0x42DB95`, `0x431D56` | rewritten `rel32` | the radii, into and out of `namepoints.bin` |
| `0x2942FB` | rewritten `rel32` | one rebuild after the world has finished loading |
| `0x312321`, `0x3125DC` | repointed | the ground dot grid's radius and chord |
| `0x312711`, `0x312730` | repointed | its step, so a big city does not walk 200×200 points |
| `0x31274A` | fourteen bytes rewritten | its x-loop end bound, which could not be repointed |

### The search is replaced, the creation is not

The hook on `0x3305F0` runs the plugin's own loop — per-city radius, circle or
square — and returns what it finds. When it finds nothing **and `create` is
set**, it calls the original, which is the game's own "invent a name, allocate,
push, reassign the neighbourhood" path. Nothing about creating a city is
reimplemented here, which is the point: a city this plugin causes to exist is
indistinguishable from one the player made with the editor tool.

For that to work the original's own search has to fail, or it answers from the
vanilla 1000 m and never reaches the creation half. Hence the `0.0f`:

```
comiss xmm7, xmm0      ; xmm7 = 0.0, xmm0 = the squared distance
jbe    skip            ; 0 <= d for every d >= 0, and for NaN
```

The literal at `0x90B214` is **repointed, not overwritten** — it sits in the
shared pool with unrelated readers, exactly as `480.0f` does for `walking`.
Rewriting one displacement touches one read.

**With every city on the default 1000 m circle the replacement is bit for bit
the vanilla decision**: same squared 3D distance, same strict `<`, same `1e10`
starting point, same `+0x118` skip. Installing the plugin and changing nothing
changes nothing.

### Why the reassignment had to go too

`0x32FEA0` does the bookkeeping when a building changes hands: erase it from the
old city's vector, `push_back` into the new one, renumber both. Two things made
it unusable as it stands.

Its box is the fixed 1000, so a city raised past that would never see the
buildings it should now hold. And it reads the *old* city without checking it
for null:

```c
if (old != building->city) {
    plVar12 = *(longlong **)(old + 0x1a8);   // old may be 0
```

Vanilla never has a building without a city, so vanilla never hits it. This
plugin can produce one — for exactly as long as it takes to adopt it — so the
null-safe version is not optional.

The replacement uses the game's own `push_back` at `0xB0E90` rather than growing
the vector itself. **That buffer is freed by the game's allocator, so it has to
have been allocated by it.** Erasing allocates nothing and is done in the
plugin.

### Where the radius lives

Nowhere in the game's structures — there is no spare field whose meaning is
known, and the save record is a fixed `0x130` bytes the loader reads field by
field:

```
+0x000  float x, y, z
+0x00C  wchar_t name[128]
+0x10C  u32          -> np+0x110
+0x110  u32          -> np+0x114
+0x114  u8           -> np+0x118
+0x118  u32 buildingCount
+0x11C  u32 hasBlob
+0x120  16 bytes written as zero and never read back
```

followed by `buildingCount` building indices and, when `hasBlob`, `0x80` more
bytes.

So the plugin keeps its own table keyed by `NamePoint*` and **appends its own
block to `namepoints.bin` after everything the game writes**, keyed by position
in the vector — which is the same key the game's own loader uses (record *i* →
`vector[i]`). The game reads exactly as many records as there are name points
and then stops, so the block is invisible to it: **a save written with this
plugin still loads without it**, with every city back on 1000 m.

```
"TSMCITY1"  u32 version=1  u32 count   then count * { float radius; u32 flags }
```

The two call sites are rewritten rather than hooked, because both hand over the
`FILE*` that is needed and neither prologue is worth stealing: `call 0x333440`
at `0x42DB95`, `call 0x3337D0` at `0x431D56`. Same technique as the deposits
plugin's world-map save. `fread`/`fwrite` come out of the **game's** import
table — a `FILE*` from the game's CRT means nothing to the plugin's own.

**`LoadNamePoints` is not handed the game object.** It ignores its first
argument and reads the vectors as globals, so the call site never loads `rcx`;
the plugin uses the static at `0x9D4F10` instead, and verifies that address
against the `lea` at `0x43966A` before anything else runs.

### Load order, and the one rebuild

Loading a world does the city work in three steps:

1. cities are created (an import or a new map creates one per `civ_kirche*` /
   `civ_kostolik` / `civ_dedina7a`; a save already has them);
2. `LoadNamePoints` at `0x431D56` restores names, positions and building lists —
   **this is where the plugin reads its block**;
3. `0x439642` walks every building whose `+0x108` is still null and calls
   `FindCity(pos, create=1)` — **the base game already creates a city for an
   orphan**, and that call now goes through the plugin.

Step 1 runs on default numbers, because the radii are not known yet, so one full
rebuild is needed afterwards: every building's city recomputed, every city's
list refilled in building order, everything renumbered.

**It cannot be done in step 2.** Version 1.0 did exactly that and faulted, every
load, at `0x3303FF`:

```
FAULT    cities load: code C0000005 at ...3303FF
0x3303F3  mov rcx,[rax+0x318]        ; the building's type record
0x3303FA  test rcx,rcx
0x3303FD  je   <bbox path>
0x3303FF  mov esi,[rcx+0x200]        ; <- here
```

that is the game's own renumbering reading a type record off a `building+0x318`
which is not one yet: **the buildings are still being assembled while
`LoadNamePoints` runs**. The non-null test passes on a pointer that is merely
uninitialised.

So the rebuild is deferred to the end of the world load. `0x430F20` has exactly
one caller, `0x2942FB`, and its `rel32` is rewritten the same way the two
serialisers' are — no prologue to relocate, and a refusal rather than corruption
if the game moves. By the time it returns, step 3 has run and every pointer is
real.

### Buildings that end up outside

Shrinking a city can leave a building with no city at all. After every change
the plugin rebuilds the whole assignment and then walks the buildings once more;
anything still without a city is handed to `FindCity(create=true)` — random
unused name, current date as "date established", the lot. Creating one city
adopts every orphan inside its radius through the reassignment that follows, so
the pass is over clusters, not over buildings.

`adopt = 0` turns it off and leaves them city-less, which is untested territory:
nothing in the base game produces that state.

## The window

`0x7CEDB0` builds the city window — "Date established", the delete and rename
buttons, the seven "Allow citizens to move in" checkboxes — and leaves its
bottom edge in `window+0x250`. A post-hook reads that, draws two rows there, and
writes the new bottom back, which is what makes the frame grow instead of
clipping them. Exactly `depletion`'s trick on the mine window.

| Window offset | Contents |
|---|---|
| `+0x01` | non-zero while the panel is not drawn |
| `+0x04`, `+0x08` | position; `+0x28`, `+0x2C` the scroll offset |
| `+0x20` | non-zero while the window accepts input |
| `+0x240` | the object — the `NamePoint*` on a city window |
| `+0x248` | **3 on a city window**; 0 means `+0x240` is a building |
| `+0x250` | the running Y, written at the end |
| `+0x580` | `vector<float>` of the row separator positions |

The row layout is the window's own:

```
x = (window[0x04] - DPI*15) + window[0x28] + DPI*35
y = window[0x250]                       one row is DPI*35
```

Both widgets are the game's, so the rows look like the rest of the panel:

| RVA | Widget |
|---|---|
| `0x272780` | `int Slider(game, x, y, textId, uint* value, scale, enabled, step, drag, char* hover)` — **0..100**, twenty segments plus `-` and `+`, returns 1 on the frame it changed |
| `0x395650` | `int Checkbox(game, x, y, uint* value, const wchar_t* label, colour, font, size, labelDy, live, 1, 0)` — draws its own label, toggles `*value` itself |
| `0x273AF0` | the plainer checkbox, label by text id. **Not used, see below** |

A `textId` of `-1` skips the slider's label, so the plugin prints its own with
`C3D_FONTMANAGER::PrintLeftUnicode` and needs no new language entry; the
checkbox takes its label as a string outright. The slider is percent-only, so
`radius_min`/`radius_max` in the ini are what its ends mean and also its
resolution.

**Which checkbox matters.** The shape row used `0x273AF0` first and was reported
dead in game while the slider worked, which is exactly the shape of the
difference between the two:

```c
0x273AF0   if (hit && game[0xAD74] == -1) { if (latch) { latch = 0; toggle; } }
0x395650   if (hit && live)               { if (latch) {            toggle; } }
```

`game+0xAD74` is the widget currently being edited or dragged. Anything that
claims it makes `0x273AF0` inert — and the window dispatcher clears
`window+0x20` whenever it is set, at `0x6EA214`:

```
0x6EA214  cmp dword [rdi+0xAD74],0
0x6EA21B  jl   +4
0x6EA21D  mov  byte [rsi+0x20],0        ; the window stops accepting input
```

`0x273AF0` also **consumes** the click latch, which the labelled one leaves for
whoever is drawn after it. So the shape row now uses `0x395650` — the widget the
city window itself draws one row above, for "Allow citizens to move into this
city", argument for argument off its own call site at `0x7CF7F9`.

It comes with one layout quirk: **it draws above the y it is handed**, the box
at `param_3 - DPI*25` and the label at `param_3 - DPI*37`, which is how the
vanilla row is positioned relative to its own running Y. A row placed at the
running Y therefore lands a third of a row high and sits on the slider — the
reported overlap. The 25 is added back so the box lands on its own line, and
`checkbox_dy` is there to nudge it.

The reassignment does **not** run per frame while the slider is being dragged.
The change sets a dirty flag and the pass runs on the first frame the value
stops moving — including the frame the window closes, which is why the flush
sits in the hook rather than in the row drawing.

## Shape

`square = 1` replaces the circle with `|dx| < r && |dz| < r` — a square of side
`2r`, so it **circumscribes** the circle of the same radius. Switching shape can
therefore only ever take buildings in, never drop them. The circle stays three
dimensional because that is what vanilla measures; the square is flat, because a
square city with a height is not a thing anybody wants.

Which city wins when two contain the same building is unchanged: the nearest
centre, by plain distance, whatever the shapes and radii involved.

## The dots on the ground

The blue grid the game paints under a city while its window is open, in
`0x30D100` at `0x3125A0`. It is a **scan**, not a shape:

```c
if (window[0x248] != 3) return;                 // the first window, and a city's
np = window[0x240];
for (x = np.x - R;  x <= np.x + R + 5;  x += 50) {
    h = sqrtf(R*R - (x - np.x)*(x - np.x));      // the circle's chord at this x
    for (z = np.z - h;  z <= np.z + h + 5;  z += 50)
        if (FindCity(&(x, ?, z), 0) == np)  draw a dot;
}
```

**The dot is drawn by asking the search**, so it was already honouring a
per-city radius the moment the search did. What was wrong is only the region it
walks — the vanilla 1000 m circle — which is why a bigger radius showed no more
dots and why a square looked exactly like a circle: the corners are never
sampled. Reported as *"галочка про квадратную область ставится, но точки не
меняются"*, and that is precisely the shape of it.

Four of the five numbers are ordinary repoints:

| RVA | Reads | Role |
|---|---|---|
| `0x312321` | `0x90B094` = 1000 | `x` loop start, into **xmm11** |
| `0x3125DC` | `0x90B214` = 1e6 | `R*R` for the chord, into **xmm11** |
| `0x312711` | `0x90AA40` = 50 | the `z` step |
| `0x312730` | `0x90AA40` = 50 | the `x` step |

The step has to follow the radius, or a 5 km city walks 200×200 points and asks
the search about every city on the map for each of them, every frame. `0` in the
ini means one twentieth of the radius, never finer than the game's own 50.

**The fifth could not be repointed, and that is worth writing down.** The `x`
loop's end bound reads the same 1000 into **xmm9** at `0x312741` — and xmm9
survives the block: `divss xmm0,xmm9` at `0x314CC2`, ten kilobytes later in the
same function, divides an unrelated integer by it. The two `movss
xmm9,[0x90B094]` at `0x312760` and `0x31276B` are there to put 1000 back on the
paths that skip the loop, which is the proof. Repointing the read would have
quietly changed that division every time a city window was open.

So the **comparison** is rewritten instead of the constant. Fourteen bytes at
`0x31274A`:

```
F3 41 0F 58 C1     addss  xmm0,xmm9        ->   F3 0F 58 05 <disp32>   addss  xmm0,[our R]
F3 41 0F 58 C5     addss  xmm0,xmm13            41 0F 2F C0            comiss xmm0,xmm8
41 0F 2F C0        comiss xmm0,xmm8             90 90
```

xmm9 keeps its 1000, and the five-metre slack `xmm13` added goes with it — which
is invisible against a fifty-metre grid. xmm11 was checked the same way and has
no reader downstream, which is why the other three are repoints.

For a square the scan radius is `r * sqrt(2)` so the circle reaches the corners;
everything outside the square is rejected by the search anyway, so the region
only ever has to be a **superset**.

## Settings

`plugins/cities.ini`, all of it optional, all of it defaulting to the base game.

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | |
| `window` | 1 | the two extra rows |
| `adopt` | 1 | give a city to whatever falls outside every one |
| `radius` | 1000 | what a city that has never been touched uses |
| `square` | 0 | and its shape |
| `radius_min`, `radius_max` | 200, 5000 | what the ends of the slider mean |
| `step` | 2 | percent per click of `-` / `+` |
| `label_x`, `widget_x` | 10, 200 | where the rows sit, in unscaled layout units |
| `checkbox_dy` | 25 | pushes the shape row back down onto its own line |
| `overlay` | 1 | make the ground dot grid follow the radius and the shape |
| `overlay_step` | 0 | its spacing in metres; 0 follows the radius |
| `probe` | 0 | the city table after every load, and one line per frame while a city window is open |

## What went wrong the first time

Version 1.0 was run in game and produced three results, two of them faults.

**It crashed on every load**, in the rebuild it did from `LoadNamePoints` —
written up under *Load order* above. The rebuild is deferred now.

**The dots ignored the shape**, which turned out not to be a bug in the shape at
all but the scan region — written up under *The dots on the ground*.

**The slider was reported as snapping back to the maximum.** That one is not
explained yet. The log says the value *was* changing — `cities reassigned: …`
fired twenty-odd times per drag, which only happens when the widget reports a
change — so the mechanism works and something puts it back. Two things have
changed on the suspicion of it and one on certainty:

* the reassignment no longer runs while the mouse button is held. It used to run
  on every second frame of a drag, which is a full pass over every building and
  every city each time. A slider that stutters and lands somewhere else is
  exactly what that feels like.
* `CfgPurge` no longer runs on a slider release. It is quadratic in the number of
  cities and it is the one function that can take a setting away from a city that
  is still on screen; once a save is enough.
* `probe = 1` now prints, every frame a city window is open,
  `probe window  cfg 3/36  r=2456 circle  pct 100->47  sq 0->0  n=0  live=1
  busy=-1 latch=0 held=0` — the value on both widgets and **every gate they are
  gated on**, so a run says which gate is shut rather than only that nothing
  happened.

The second run settled the first two and reported the third fixed — and broke
the checkbox, which is what pointed at `game+0xAD74` and moved the shape row
onto `0x395650`. See *Which checkbox matters* above.

## Testing it

What to look for, in order:

1. **The log, at startup.** `hook ok  city search`, `hook ok  city reassign`,
   `hook ok  city window`, then `cities   search limit  rva 0x330658 now reads
   ... (0)` and the two box lines. A `refusing` on any of them means a different
   build, and the plugin then leaves that half alone.
2. **Nothing changed.** With the defaults, load a save and confirm every city
   holds the same buildings it did — the search is supposed to be bit-identical
   at 1000 m and circular.
3. **The window.** Open a city (click its name on the map). Two rows under the
   checkboxes: `City radius: 1000 m (37 building(s))` with a slider, and
   `Square city area` with a checkbox. The frame should have grown by two rows,
   not clipped them.
4. **Growing one.** Drag the slider right, let go, and read the log:
   `cities   reassigned: 0 building(s) outside every city, 0 new city(-ies), N
   total`. The building count in the row should go up, and buildings that used
   to belong to a neighbour should change hands.
5. **Shrinking one.** Drag it left far enough to strand something. The log
   should say a non-zero orphan count and a non-zero "new city(-ies)", and a new
   name should appear on the map where the stranded buildings are.
6. **The square.** Tick it and watch the count change at the corners.
7. **Save and reload.** The radii and shapes must come back. `probe = 1` prints
   the whole table on load, which is the quickest way to see it.
8. **Take the plugin off.** The save must still load, with every city back at
   1000 m.

The things most likely to be wrong, in the order they are worth suspecting:

- **the two rows land outside the frame.** `window+0x250` is read for the frame
  somewhere this project has only inferred; if the rows draw but the panel does
  not grow, the frame comes from elsewhere and `label_x`/`widget_x` will not
  help.
- **the slider is the wrong width for the panel.** It needs about 225 unscaled
  units and the panel is about 500 wide; `widget_x` moves it.
- **a large radius is slow.** The reassignment is buildings × cities and runs
  once per change, not per frame — but `RebuildAll` walks everything, and on a
  full map that is a visible hitch.
- **adoption loops.** Every creation must place at least the building that asked
  for it, or the pass spins; the plugin forces that building into the new city
  if the game's own reassignment did not.
