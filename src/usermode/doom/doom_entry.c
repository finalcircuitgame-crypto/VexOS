#include <stdlib.h>
#include "doomgeneric.h"

void doom_user_main(void) {
    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char arg2[] = "freedoom1.wad";
    char *argv[] = { arg0, arg1, arg2, 0 };
    doomgeneric_Create(3, argv);
    for (;;) {
        doomgeneric_Tick();
        DG_SleepMs(1);
    }
}
