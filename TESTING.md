# Testing

Use a **dev** package (`package.ps1 -Dev`) so the UE4SS window and the **Islands MP** tab are available. Release packages have the console and GUI turned off.

## Local tests (one PC)

All of these run on the game thread and go through the same receive path a real remote player uses.

| Test | How | Expect |
|---|---|---|
| Loopback clone | **Start loopback clone**, then walk, run, jump, glide, crouch | A copy of you 3 m to the side, 120 ms behind, in your outfit, with matching animation, head look, wings and trail. |
| Stress test | **Stress test: 8 random clones** | Eight clones in a ring with random looks, titles and levels that follow your moves. Watch the FPS. |
| Puzzle cursor | Loopback clone on, then open a grid puzzle | Your own name follows your mouse on the panel. Name cards hide while you're in the puzzle. |
| Static clones | **Spawn static clone** a few times | Standing copies of you with your name card. |
| Map change | With clones up, travel or reload | Old puppets are dropped without crashing and respawn after you're back in. |
| Appearance capture | **Log my appearance** | `UE4SS.log` lists the parts, overrides and effect parameters. |

Useful log lines start with `[IslandsMP]` in `Binaries\Win64\UE4SS.log` (UE4SS writes the log even with the console off).

## Two players

Both need the same version (lobbies are filtered by version). One picks **PLAY WITH FRIENDS** and invites the other through the Steam overlay, or both pick **PLAY ONLINE**. The lobby owner relays packets for 3+ players.
