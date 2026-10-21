// Online play: deterministic lockstep on controller input.
//
// Every peer runs the whole game; only pad state crosses the network. The runtime is
// deterministic given its inputs (input_log.h: a replay is exact), so peers that start from
// the same state and see the same pad state at every SI poll run the same game. The SI's
// 120 Hz poll tick (hw/si.cpp) is the unit of the lockstep: at tick T each peer sends its
// own controller's state for tick T + delay and waits until every player's state for T has
// arrived, and all four channels then read those states until the next tick.
//
// The one thing about the run that depends on the host is the clock's catch-up
// (clock_pace in cpu.cpp): a host that falls behind real time jumps virtual time forward,
// which the game sees. In a session no peer catches up on its own; the host measures its
// own lag when it produces its input for a tick and sends the jump with that tick, and
// every peer makes it at that tick. Each peer's input log records the session's pad states
// and jumps as usual, so any peer's log replays the session exactly.
//
// The topology is a star: clients talk to the host only, and the host relays. Only the
// host needs a port others can reach. Before the game boots, each client sends the host
// its build's identity (the recompiled function table and every module's fingerprint), and
// the host sends back its memory card, the RTC's start time and the client's channel.
// Every peer then boots on the same card and clock.
//
// While the game runs, each peer hashes guest RAM every few seconds and the host compares:
// a mismatch is a desync, reported on both sides (the run goes on; the input logs are what
// to compare).
#pragma once
#include "hw/pad.h"
#include <string>

// Host a session on `port` for `players` players (2-4) including the host, which plays
// channel 0. `card` is the host's memory card, sent to every client. Blocks until every
// client has joined and received it; false on a network error.
bool netplay_host(int port, int players, int delay, const std::string& card);

// Join the session at `host` ("address:port"). The host's card is written to `card_out`,
// which the frontend then uses as the memory card. Blocks until the host starts the
// session; false if it cannot be joined.
bool netplay_join(const char* host, const std::string& card_out);

// Whether a session is in progress.
bool netplay_active();

// The SI's poll tick, on the guest thread: sends the local controller's state (`local`) for
// a later tick and waits until this tick's states have arrived.
void netplay_tick(const PadState& local);

// The state channel `chan` reads this tick. True while a session is in progress.
bool netplay_pad(int chan, PadState& s);

// Leave [addr, addr + size) of guest RAM out of the desync hash: memory a game's hook writes
// for the picture alone, which peers drawing differently (one at 16:9, one at 4:3) hold
// differently without having diverged. Call before the session starts.
void netplay_hash_exclude(uint32_t addr, uint32_t size);
