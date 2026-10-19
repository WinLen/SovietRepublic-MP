# Day and night, and the calendar

The base game keeps two clocks and runs them at different speeds. The date in
the corner advances once per minute of game time; the sun does not. A full
day/night cycle takes **thirteen calendar days**, and the night part of it lasts
two of them, so a fortnight of dates goes by between one sunrise and the next.

The `daynight` plugin compresses that thirteen-step cycle into one calendar day.
It patches nothing and reimplements nothing: the same `weathers/*.ini` files,
the same fades, the same proportions — the game's own state machine, handed a
date derived from the position inside the current day instead of from the
calendar.

## The calendar

Three fields of the world object, and one function that owns them:

| Field | What |
|---|---|
| `game+0x590` | `int`, day of the year, `0..364` |
| `game+0x594` | `int`, the year |
| `game+0x59C` | `float`, how far into the current day we are, `0..60` |

The day tick is the whole of it: every frame it adds `C3D_TIMER::PowerTime` to
`+0x59C`, and the moment that passes the `60.0f` at `0x90AA78` (this build) it
zeroes the field, increments `+0x590`, and wraps at `365` (`0x16D`) into
`+0x594`:

```c
time = PowerTime(timer, 0.001f, false, false) + game[0x59C];
game[0x59C] = time;
if (time <= 60.0f) return;
game[0x59C] = 0.0f;
...
game[0x590] += 1;
if (game[0x590] == 365) { game[0x594] += 1; game[0x590] = 0; }
```

**One calendar day is 60 units of scaled game time and nothing else.** That the
day counter really is a day of the year rather than a tick counter is settled by
the season test, which compares it against `25`, `50`, `52`, `128`, `245`,
`247` and `326` — the winter boundaries, per climate.

The day tick's rollover branch is also where a long list of once-a-day systems
live — pollution decay, loan repayment, notification/offer expiry, the price
recompute, the random-event roll, a per-city tick, year rollover — all called
from inside it and from nowhere else. That list matters below: it is the whole
reason slowing the calendar is not, by itself, a safe thing to do.

## The other clock

The day and night themselves come from a separate state machine, the weather
tick, called once per tick from the same frame function as the day tick. It
steps once per **calendar day**:

```c
phase = game[0x590] % 13;
```

| `phase` | Lighting | `game+0xE74`, the night factor |
|---|---|---|
| 0–5, 11, 12 | `day1.ini`, or `overcast1.ini` if the roll said so | 0 |
| 6 | `sunset2.ini` | 0 |
| 7 | `night.ini` | ramps 0 → 1 over the first quarter |
| 8 | `night.ini` | 1 |
| 9 | `sunset2.ini` | ramps 1 → 0 over the first quarter |
| 10 | `day1.ini`, forced clear | 0 |

"the first quarter" is `game+0x59C` against a `15.0f` constant — a quarter of a
calendar day.

The two outputs are:

| Field | What |
|---|---|
| `game+0xE70` | `int`, which `weathers/*.ini` is current — 0 day, 1 sunset, 2 night, 3 overcast |
| `game+0xE74` | `float`, the night factor. Building windows, street lights and **their electricity consumption** are switched on by it |

Changing `+0xE70` loads the two lighting files and sets up a cross-fade:

| Field | What |
|---|---|
| `game+0xEC0`, `+0xEC8` | the two `C3D_LIGHTING` being blended |
| `game+0xED0` | `float`, how far into the blend |
| `game+0xED4` | `float`, how long the blend takes — a fixed `15.0f` |

A separate function advances `+0xED0` by the same scaled clock and finishes the
blend when it passes `+0xED4`.

## Exactly three places divide by thirteen

Scanning `.text` for `0x4EC4EC4F` — what MSVC emits for a signed `% 13` — finds
three sites and no more:

| Site | In | Reads |
|---|---|---|
| — | the state machine, `0x333BD0` | `game+0x590` |
| — | the light factor, `0x5CFC40` | `game+0x590` |
| — | the cross-fade | `0x9D54A0` — **not** the world object |

The third is a separate clock with its own day at `0x9D54A0` and its own
time-in-day at `0x9D54AC`, used where there is no calendar at all. It is left
alone.

**The light factor is the same thirteen-phase logic written out a second
time** — same `% 13`, same cases 6…10, same `15.0f` quarter — returning a
float: how lit the world is. It is read by the renderer and by two of the
electricity functions. Patching only the state machine would have moved the
sky and left the street lights on the old thirteen-day schedule. A second copy
of the same logic is normal in this executable; see
[07-pitfalls.md](07-pitfalls.md).

## What the plugin does

Both functions are hooked, and each is handed a day and a time-in-day
re-derived from the position inside the current calendar day:

```
u  = ((day mod cycle_days) * 60 + time) / (cycle_days * 60)   0 .. 1
k  = floor(u * 13)                                            0 .. 12
t' = (u * 13 - k) * 60                                        0 .. 60
d' = the day nearest `day` with d' % 13 == k, in the same season as `day`
```

`t'` is rescaled back onto the game's own `0..60`, so the `15.0f` the fade
compares against still means *the first quarter of this phase* and every
proportion in the vanilla machine survives.

### `d'` has to land in the same season as the real date

The state machine does not only take `day % 13` off the field. It also hands
it to the snow-season test, and compares it against the winter boundaries per
climate. So the day we invent has to be believable as a date, not merely as a
residue.

A residue class modulo 13 has a representative every thirteen days. Version 1.0
took **the next one at or after the real date**, so as the phase advanced
through a single calendar day `d'` swept the whole window `day … day+12` — and
near a winter boundary that window straddles it, so the weather flipped in and
out of winter thirteen times a day.

The plugin walks the representatives outward from the real date — `day+m`,
`day+m−13`, `day+m+13`, … ordered by distance — and takes the first one the
game's own season test puts in the **same season** as the real date. Every
season in that function is at least a hundred days long and the representatives
are thirteen apart, so a matching one always exists; the answer is within seven
days of the real date almost always and within thirteen at worst.

The test is asked on a **scratch object of the plugin's own**, the same trick
the light factor uses: the season test's 120-ish bytes reference their argument
exactly twice, both `[arg+0x590]`; everything else it reads is an absolute
global. So the world object is never written to answer a question about it. If
the function's bytes do not match at startup the plugin says so and falls back
to the nearest representative — version 1.0's behaviour minus the systematic
forward bias.

**With `cycle_days = 13` the mapping is the identity**: `k` is `day % 13`, `t'`
is `time`, `d'` is `day`. The plugin short-circuits that case rather than
computing it, so 13 is bit for bit the base game and is the control to test
against when something looks wrong.

### The two hooks deliver the lie differently, deliberately

| Site | Bytes stolen | How |
|---|---|---|
| weather tick | 15 | **Writes** to the world object — the weather index, the night factor, two flags — so it gets the real object with `+0x590` and `+0x59C` temporarily rewritten and put back afterwards |
| light factor | 16 | **Reads** only. Handed a scratch object of the plugin's own |

The scratch is safe because the function's bytes were read instruction by
instruction: it touches `+0x5C4`, `+0x590`, `+0x59C`, `+0xE70` and `+0xE28` of
the object and nothing else, everything else it reads being an absolute global.
The scratch is `__declspec(thread)` and zero-initialised, so the render thread
and the simulation thread cannot be handed the same one, and the world object is
never written from the render side at all.

The write-and-restore on the weather tick is safe because it runs on the
simulation tick immediately after the day tick that owns those two fields — the
frame function calls the day tick and then the weather tick, back to back — so
nothing else is looking at them in between.

The restore is unconditional, and that includes overriding the one write the
original makes to `+0x59C` itself, under the editor flag at `+0x1090`. It
zeroes the field when the weather changes under that flag — which was harmless
while a phase *was* a calendar day, and would now throw away most of a day of
the calendar every time the weather turned.

### The phase boundary has to look like a day boundary

The game gets that for free: a phase *is* a day, and the day tick leaves
`+0x59C` at exactly `0.0f`. Two things key on it — the random weather roll
(`if (time == 0.0f && weather == 0)`) and the "clear the overcast" edge at
`time` crossing `40.0f`. So the plugin forces `t' = 0.0f` on the first tick of
each new phase rather than letting it be whatever the float arithmetic produced.

The very first tick of a session is **not** a boundary and passes the real
figure through — otherwise `cycle_days = 13` would differ from vanilla by one
tick after every load, which is exactly the kind of "almost" the short circuit
exists to avoid. The state machine's own second argument is the signal: it is
non-zero only on the world-load path, and the remembered phase is forgotten
there.

### And then the cycle runs thirteen times too fast

Compressing the cycle into one calendar day does not slow the calendar. The
cycle therefore comes out **thirteen times faster than the base game's**, and a
calendar day is only a few seconds of real time — measured at 2 to 6 seconds in
the first session this ran in.

That measurement does not need a probe. The day tick calls the price recompute
on days 5, 10, 15, 20, 25 of the month and at every month change, and
`resources` prints the whole table when `price_report = 1`. Four blocks
9.96 s, 30.1 s and 18.35 s apart is five in-game days each: 2 s, 6 s, 3.7 s per
day.

So the whole thirteen-step cycle — sunset, night, sunrise — goes by in a couple
of seconds, and it reads as *night tried to start and changed its mind*, because
that is exactly what it looks like.

`day_scale` is what makes a calendar day last longer. **What it stretches has
gone through three different answers, and the third is the one worth reading
carefully — it is the current default and it is not simply "the fix that came
last", it deliberately picks the trade-off the other two got wrong in opposite
directions.**

### Version 1.0: stretch the calendar alone

The first implementation gave back most of what the day tick had just added to
`+0x59C`:

```c
raw = game[0x59C];                                    // what the day tick left
if (raw >= written) out = written + (raw - written) / scale;
else                out = raw;                        // it rolled over
game[0x59C] = written = out;
```

It works, in the narrow sense that the calendar really does advance
`day_scale` times slower — and **nothing else does**. Every rate in this game
is integrated from `C3D_TIMER::PowerTime` per frame, and the day tick's own
rollover branch calls a long list of once-a-day systems, several of which
integrate real elapsed time rather than just bookkeeping the date. Slowing only
the calendar field pulls those apart by a factor of `day_scale`. Three were
found in the disassembly after being reported from a real session:

| Symptom | Where it comes from |
|---|---|
| Pollution runs away and never settles | the daily pass (`0x4D5F50`, this build) subtracts a flat `0.06` and `0.005` from every cell of the grid at `game+0x120A8`, **once per calendar day** — its only caller is the day tick's rollover branch. Emission is per frame. `day_scale` times the emission between two subtractions |
| Winter kills the population | the same daily pass accumulates each residential building's exposure at `building+0x11B0` from a sample capped at `3.0`, clamped to `1.0`. With the grid running away every home pins at full exposure and citizens sicken. Winter is where it peaks, because buildings with no district heating burn fuel locally and that is one of the emitters. The season boundaries are days of the year, so each winter also lasts `day_scale` times longer on the wall clock |
| A loan never repays | the loan list is the `0x28`-byte vector at `game+0x10BD0`; a per-loan function decrements the **term in days** at `record+0x10`, compounds the interest and pays `balance / days-left`. The daily driver (`0x4B9660`) runs it once per calendar day. `day_scale` times fewer payments out of an income that did not slow |
| Used-vehicle offers and notifications linger past when they should expire | the notification/offer countdown (`0x4CD380`) decrements a counter at `record+0x10` and frees the record at zero, once per calendar day, same shape as the loan driver |

There is no fixing these one at a time from outside — they are symptoms of one
structural mismatch — but the three functions themselves are ordinary,
self-contained bookkeeping passes that take only the world pointer and touch
only their own state. That turns out to matter: see version 2.1 below.

### Version 2.0: stretch the whole simulation clock instead

The second implementation stopped touching the calendar and scaled the shared
clock everything reads instead, so every per-day and every per-frame quantity
slowed together and the mismatch above could not occur by construction.

**The whole simulation reads one `C3D_TIMER`, the one at `0x9D4EE0`** — 337
sites reference it, the shop tick and the mine tick among them.
`C3D_TIMER::PowerTime` is four instructions:

```c
if (this[0xC] && !ignorePause) return 0;
if (this[0xD] && !realTime)    return 0;
return v * K / (realTime ? this[4] : this[0]);
```

`this[0]` is the frame rate game time is divided by; `this[4]` is the real one,
which is why a `realTime` caller must never be scaled — that argument is how the
engine asks for wall-clock time on purpose. And **the game's own speed control
is a multiplication of exactly that field** — the engine's own frame function
calls `C3D_TIMER::Start` on the timer every frame and then multiplies `this[0]`
by `0.35`, `0.05` or `0.01` for the three speeds, or by `3`, `5` or `1000` for
the slow modes. So scaling it is not a new mechanism — it is one more speed
step, and the engine already ships a mode that divides the step by a thousand,
which is the safety argument for a factor of thirteen.

It is done as an **import swap on the three `C3D_TIMER::Power*` the executable
imports** — `Power`, `PowerTime`, `PowerKmh` — rather than as a write to the
field, because a wrapper is stateless (cannot compound if a frame runs the site
twice, cannot be missed if a frame skips it), and because scaling the time and
not `PowerKmh` (vehicle speeds) would be a worse desynchronisation than the one
being fixed — the plugin resolves all three slots before patching any and puts
back whatever it patched if one refuses.

The site is verified as a unit before anything is swapped: the day tick's own
`lea rcx,[sim timer] / xor r9d,r9d / xor r8d,r8d / call [PowerTime]`, with both
displacements resolved and compared rather than matched as bytes.

**This is the honest answer and it works exactly as advertised**: in-game the
economy is bit for bit the base game's, only slower on the wall clock, and
raising the game speed cancels the slowdown exactly. The cost, reported after
watching it in game, is that **everything else the shared timer drives also
slows by the same factor** — walking, vehicle speed, construction, every
animation. At the default thirteen-times stretch that does not read as "a
longer day," it reads as the whole world stuck in slow motion, and vehicles —
already the slower of the two per unit in the base game — become slower than
a walking citizen.

### Version 2.1: stretch the calendar alone again, but drive the three broken systems

This is the current default. It goes back to touching only `+0x59C` — nothing
about `C3D_TIMER` — so walking, vehicles, construction and every animation keep
the base game's own real-time pace, which is what "the day is slower, not the
world" actually has to mean. What makes this safe where version 1.0 was not is
that the three functions identified as broken are ordinary, self-contained
passes over state that already exists, callable at any time with just the world
pointer — so instead of letting the day tick's own, now much rarer, rollover be
their only trigger, **the plugin calls them itself, directly, at the cadence
the un-slowed calendar would have used**:

```c
delta = raw - g_written;             // the true PowerTime the day tick just added
g_dailyAccum += delta;
while (g_dailyAccum >= g_dayLen) { g_dailyAccum -= g_dayLen; crossings++; }
if (a real rollover happened this frame) crossings--;   // the day tick paid for one
for (crossings) { pollution(world); loans(world); notices(world); }
```

`g_written` is the same bookkeeping `SlowTheCalendar` already keeps to know how
much to give back; the true, un-slowed delta the day tick added this frame falls
out of comparing `raw` (what the day tick just wrote) against it. The
accumulator is independent of what actually gets left on display, so pollution
decays, loans get paid and notices expire at essentially the base game's own
real-time rate regardless of how rarely the displayed calendar rolls over — one
call is skipped whenever a real rollover happens to land on the same frame,
because the day tick's own code already ran that one.

**What was NOT added to this list, and why:** everything else the day tick's
rollover branch calls — the price recompute, the random-event roll, the
per-city tick, the weather forecast roll, year rollover, snow-cover clearing —
is date bookkeeping, not a real-time integrator: it reads or writes state keyed
to *which day it is*, not *how much time has passed*, so running it at the
calendar's own (slowed) pace is correct, not merely tolerated. Compensating
those too would be wrong in the other direction — a random event or a price
update firing thirteen times more often than a real calendar day warrants. The
random-event roll (gated on `game+0x5CC`) and the fire roll (`game+0x5D0`) were
considered and left out for the same reason **and** because both are internally
gated on settings this plugin cannot see without reading them separately; a
manual call from outside those gates could fire an event the player disabled.

Resolution follows the same pattern as the snow-season test: a prologue-byte
check against each address, refuse-and-log per function rather than for the
whole plugin, and never hooked — the day tick's own calls are left completely
alone, this just adds extra ones. A daily pass that faults on a catch-up call
disables catch-up for **all three** (they are cheap insurance against one
failing silently while the others keep running) and falls back to the
functions running at the slowed calendar's own rate — version 1.0's original
behaviour for whichever of them that turns out to be.

### `slow = world` remains, for the honest uniform slowdown

`slow = world` is version 2.0's mechanism exactly, kept for anyone who wants
that trade-off on purpose — a deliberately cinematic, uniformly slower pace
where "everything, including you, is unhurried" is the point rather than a side
effect. `slow = none` is `day_scale = 1`: the cycle still compresses to one
calendar day but nothing is stretched, so it is a few seconds long and mostly
useful as a sanity check that the compression itself works.

`auto` is `13 / cycle_days` in every mode — the value that leaves one whole
cycle taking exactly as long in real time as the base game's did — and it is
`1.0` at `cycle_days = 13`, so that case still changes nothing regardless of
`slow`.

### The cross-fade

`game+0xED4` is the one number that does not scale itself: the weather-change
code writes a fixed `15.0f` into it on every change. That was a quarter of a
calendar day, and a phase is no longer a calendar day, so the plugin rewrites it
after every tick to the same fraction of the new phase. `fade = 0` leaves it
alone.

The day scale belongs in that figure regardless of `slow`, because the
cross-fade's own clock is a separate timer that neither mode touches — a phase
lasts `day_scale` times longer in the units the fade is measured in either way,
so

```
fade = 15 * cycle_days * day_scale / 13
```

which with `day_scale = auto` comes back to exactly the vanilla `15`.

## Settings

`plugins/daynight.ini`:

| Key | Default | What |
|---|---|---|
| `enabled` | 1 | |
| `cycle_days` | 1 | calendar days per full day/night cycle. 13 is the base game exactly |
| `day_scale` | auto | how many times longer a calendar day lasts. `auto` is `13 / cycle_days`; `1` leaves the clock alone |
| `slow` | **calendar** | what `day_scale` stretches. `calendar` touches only the date, and drives pollution/loans/notices itself to stay synced — see above. `world` scales the whole simulation clock, uniformly, including movement and vehicles. `none` is `day_scale = 1` |
| `vehicle_scale` | 1 | vehicle speed only, independent of everything below. `> 1` slower, `< 1` faster |
| `sim_scale` | 1 | everything the shared clock drives except vehicles — walking, construction, production, animation — independent of the calendar and of `vehicle_scale`. Same `> 1` / `< 1` convention |
| `offset` | 0.0 | rotates the cycle, as a fraction of one cycle |
| `fade` | 1 | scale the lighting cross-fade to the new phase length |
| `probe` | 0 | a line per phase change, per lighting change and per date, plus a periodic figure |

`offset` exists because the cycle begins with daylight, so with `0` the dark
part lands a little past the middle of the calendar day. There is no clock in
the game's interface for that to disagree with — it only decides where in the
day the night falls. Anything other than `0` also takes `cycle_days = 13` off
its exact-vanilla short circuit.

### `vehicle_scale` and `sim_scale`: rebalancing, not day/night

These two exist for a different complaint than everything above: even once
`slow = calendar` stopped the whole world running in slow motion, vehicles and
pedestrians did not necessarily feel correctly paced *relative to each other* —
a mismatch the base game already has, since a vehicle's speed comes from a
completely different conversion (`PowerKmh`) than everything else (`Power`,
`PowerTime`). Both dials are always available, in every `slow` mode, and
compose multiplicatively with whatever `slow = world` is already doing if both
are in use at once.

**Why there are exactly two, not one per category named in the code
comments.** `PowerKmh` is used for nothing but converting a vehicle's speed to
distance — every call site checked reads an adjacent vehicle-record field — so
it is a clean, single-purpose lever and it is `vehicle_scale`. Nothing else is
that clean: `Power` has 27 distinct calling functions and `PowerTime` has 311,
covering camera zoom, UI input axes (already excluded — they pass
`realTime = true`), building rotation, vehicle acceleration curves, and, mixed
in among all of that with no distinguishing wrapper, walking, construction,
production and every animation. There is no `PowerWalk` the way there is a
`PowerKmh`. Isolating citizen movement alone would mean finding and correcting
individual call sites the way `walking` patches specific distance constants,
not swapping one import — that has not been attempted, and until it is,
`sim_scale` is "everything but vehicles and the calendar" as a single dial.

## What this does not fix

The weather-change code allocates two `C3D_LIGHTING` objects on every change —
`operator new(0x270)` twice — and never frees the previous pair. That is the
base game's, and it is verified in the disassembly, not guessed. Four changes
per cycle used to mean four per thirteen days; it now means four per day.
Roughly 5 KB of leak per real minute at 1× speed, and the textures behind them
come from the managed cache and are not duplicated.

Freeing them here was considered and rejected: the memory comes from the game's
own `operator new` and the plugin has its own CRT, `C3D_LIGHTING` has a
destructor that would have to be run first, and it is not established that
nothing in `C3D_MIDDLEPOINT` holds a raw pointer to the outgoing pair for a
frame.

## Testing it

The hooks install and the cycle runs — a first session confirmed both `hook ok`
lines and a day/night that changed several times per date, which is what
produced `day_scale`. A later session, running `slow = world` (version 2.0),
confirmed the three bugs in the version-1.0 table above and their fix, but also
surfaced the slow-motion-movement problem that version 2.1 exists to answer.
**Version 2.1's own catch-up mechanism — the accumulator, the three daily
passes, and everything staying at normal speed under it — has not been watched
in game.**

1. Tick `daynight` in the launcher, set `probe = 1` in `plugins/daynight.ini`,
   and load any save. Leave `slow` at its default, `calendar`.
2. The log should carry two `hook ok` lines, `weather tick` and `light
   factor` — if the second is missing the plugin takes itself out on purpose,
   because the sky and the street lights would otherwise disagree — and then
   `daynight  calendar stretched 13.00x and NOTHING ELSE IS …` ending in
   `pollution ok, loans ok, notices ok`. Any of those three saying
   `NOT DRIVEN` instead means that function's prologue did not match this
   build and it will fall back to the slowed calendar's own, rarer rate for
   just that one system.
3. **Read the `day N -> M` probe lines first.** They give how long each date
   lasted in real milliseconds and how many phase changes fitted inside it. It
   should say 13 phase changes, and the day should take about thirteen times
   what it took before.
4. Watch a citizen walk and a truck drive **at the same time** as the date is
   advancing slowly. Both should look completely normal - this is the entire
   point of `slow = calendar` over `slow = world`, and the thing version 2.0
   got wrong.
5. Read the `N catch-up day(s)` probe lines. Over a real calendar day the
   total catch-up count should land close to `day_scale` itself - that many
   "un-slowed days" of pollution/loan/notice bookkeeping needed to run to keep
   pace with the slower, displayed one.
6. Check the economy did **not** break the version-1.0 way even though the
   calendar is slow: leave a dirty factory running and check the pollution
   overlay settles rather than climbing; take a loan and check the
   days-remaining figure falls at a normal pace; play through a winter and
   check the population holds.
7. Watch a season boundary. The `phase` probe lines carry `day 40 (as 45)` —
   the real date and the one the state machine was told. The invented one
   should stay within about a week of the real one and, at a boundary, should
   **stop crossing it**: snow should not start and stop several times inside
   one date.
8. Set `slow = world` and confirm the earlier, known trade-off: everything
   slows together, including walking and vehicles, and the log shows the
   three `C3D_TIMER::Power*` `hook ok` lines instead of the daily-pass ones.
9. Set `cycle_days = 13` and `day_scale = 1` and confirm the game is exactly
   as it was. That is the control: the mapping is short-circuited to the
   identity there and nothing is touched at all — no daily passes driven, no
   `Power*` swapped — so anything that still looks different is neither.
10. **`vehicle_scale` and `sim_scale` have not been watched either.** Leave
    `slow = calendar` at its default (so the calendar stretch is not also in
    play) and set `vehicle_scale = 0.5` alone. The log should show the same
    three `C3D_TIMER::Power*` `hook ok` lines `slow = world` uses, followed by
    `vehicle_scale 0.50x`. Watch a truck: it should now be visibly faster,
    and a citizen walking alongside it should be completely unaffected — that
    is the whole point of the split. Reset it, set `sim_scale = 2` alone, and
    confirm the opposite: citizens and construction slow down while vehicles
    stay at their own pace. Try both at once, and try either together with
    `slow = world` and a `day_scale` above 1 — the log should show the
    combined figure, and the effect should compose (e.g. `slow = world` at 13x
    plus `vehicle_scale = 0.5` should leave vehicles at roughly 6.5x, not 13x
    or 1x).
