# Online play

Deterministic lockstep on controller input (`runtime/netplay.h`): every peer runs the whole
game and only pad state crosses the network. It rests on the runtime being deterministic
given its inputs, which is what makes a replay exact (`diagnostics.md`, "Input logging and
replay").

## Running a session

The host, who plays port 1:

    <game> --host=27015 --players=4

Each other player, who plays the next free port in the order they join:

    <game> --join=<host's address>:27015

`--net-delay=<ticks>` (host only; default 6) sets the input delay in SI poll ticks, 1/120 s
each. Only the host needs a UDP port the others can reach (forwarded on its router, or on
the same network). Each peer keeps an input log as usual, and any one of them replays the
whole session exactly with `--replay`.

## How it works

- **The tick.** The SI's 120 Hz poll (`si_poll_tick`) is the unit. At tick T each peer
  sends its own controller's state for T + delay, then waits until every player's state
  for T has arrived; until the next tick every channel reads those states (`netplay_pad`,
  ahead of the input log in `pad_get`, so the log records them).
- **Star topology.** Clients send their inputs to the host; the host completes each tick
  and relays it to everyone. Each packet repeats the last 32 ticks not yet acknowledged,
  so a lost packet costs nothing but the next one.
- **The clock.** Catch-up (`clock_pace`, "The clock" in `diagnostics.md`) is the one part
  of a run that depends on the host machine. In a session no peer catches up on its own:
  the host measures its lag (`clock_lag_edges`) as it produces its input for a tick, less
  the jumps it has sent and not yet made, and sends the jump with that tick; every peer
  makes it there (`clock_apply_jump`, which logs it as clock_pace would).
- **The start.** A client sends the host its build's identity (the disc ID, the
  recompiled function table's extent and every module's fingerprint), so two builds of
  different code refuse each other. The host sends its memory card, which a client keeps
  in `saves/memcard_session.raw` and plays on, the RTC's start time and its UTC offset, so
  every peer's calendar reads the same. Both are in the input log (`# epoch`,
  `# utc offset`). A host with no card formats one at boot, stamped with the session's
  start time, and a client given none does the same, so the two cards are alike.
- **The aspect.** A game that culls to a widescreen picture (`game_aspect.h`) makes the
  aspect game state. Each client sends the host the aspect it displays, and the host sends
  back, with the start, the widest of everyone's; every peer culls to that, whatever it
  displays, so nobody's sides are empty. F9 in a session changes the picture, not the
  culling. The host sends the start until each client's first input arrives.
- **Desync check.** Every 1200 ticks (10 s) each peer hashes guest RAM at the same tick and
  the host compares; a mismatch prints `DESYNC` with the tick. The session goes on; the
  peers' input logs are what to compare. A game's hooks can make memory differ without
  the game having diverged (GPU commands recorded into a display list differ between
  peers drawing differently); the game leaves such ranges out with
  `netplay_hash_exclude`. `GCN_RAMHASH` (`diagnostics.md`) prints the same hash outside
  a session, to find them.
- **Lost peers.** Fifteen seconds without a packet from a peer the session is waiting on
  ends the session on every peer that notices.

Tested on one machine with two instances over loopback: a scripted host and an idle client
rendered the same frames through boot, menus and the start of a game, made the same 5482
clock jumps, and no RAM hash differed.

Tested again with a game whose hooks cull to the widescreen picture: a host at 21:9 and a
client at 4:3, the client dropping in as the second player mid-level. Both culled at
21:9, drew the same game, and every RAM hash matched (15 over 8400 frames).
