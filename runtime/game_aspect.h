// The picture's aspect as game state.
//
// The renderer's widescreen (gx::render_set_aspect) changes only what is drawn. A game hook
// that also widens what the game itself treats as in view -- its culling, so that the sides
// of a wider picture are not empty -- makes the aspect game state: a game that runs
// characters only while they are in view plays differently at 21:9 than at 4:3. Such a hook
// reads the aspect from here, not from the renderer:
//  - It changes only at an SI poll, on the guest thread, so it changes at the same instant
//    of the game in every run.
//  - It is input to the input log (input_pose, kind "aspect"): each change is logged, and a
//    replay sees the aspect its recording had, whatever the replaying run displays.
//  - In an online session it is the session's, the widest of the players' (netplay.h), for
//    every peer, whatever each one displays. Out of a session it follows the display.
// Hooks that change only the picture (a HUD moved to the edges) keep reading the renderer's.
#pragma once

// The aspect in force (4:3 = 1.333...). For the guest thread.
double game_aspect();

// Takes the aspect for the coming poll interval; the SI's poll tick calls it.
void game_aspect_poll();
