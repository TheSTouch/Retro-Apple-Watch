/* What SDL's core expects from its iOS backends, which the watch build of SDL
   leaves out (port/oot/cmake/watchos.cmake): SDL treats watchOS as one of
   Apple's mobile platforms. */

#include <stdio.h>

/* The joystick driver SDL lists for Apple's mobile platforms is its dummy
   driver under that name. */
#define SDL_DUMMY_JoystickDriver SDL_IOS_JoystickDriver
#include "joystick/dummy/SDL_sysjoystick.c"

SDL_bool SDL_IsIPad(void)
{
    return SDL_FALSE;
}

FILE *SDL_OpenFPFromBundleOrFallback(const char *file, const char *mode)
{
    return fopen(file, mode);
}
