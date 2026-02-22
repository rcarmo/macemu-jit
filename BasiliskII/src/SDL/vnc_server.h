#pragma once

#include "my_sdl.h"

void VNCServerInitFromPrefs();
void VNCServerShutdown();
void VNCServerUpdate(SDL_Surface *surface, const SDL_Rect &updated_rect);
void VNCServerProcessEvents();
