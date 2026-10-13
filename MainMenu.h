#pragma once

namespace IslandsMP
{
    // Adds "Play with friends" and "Play online" buttons to the title screen. Picking one
    // presses the game's own Play button and starts that kind of session once the world has
    // loaded; the regular Play button plays solo. Game thread only; call every tick.
    void TickMainMenu(float dt);
}
