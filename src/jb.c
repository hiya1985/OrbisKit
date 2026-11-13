#include "jb.h"
#include "dlog.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#ifdef USE_GOLDHEN
#include <stdint.h>
#include "GoldHEN.h"
static struct jailbreak_backup jbb;
#endif

int jb_escape(void)
{
#ifdef USE_GOLDHEN
    return sys_sdk_jailbreak(&jbb) == 0;
#else
    return 0;
#endif
}

int jb_has_root(void)
{
    DIR *d = opendir("/system");
    if (!d) return 0;
    closedir(d);
    return 1;
}

/* GoldHEN's version isn't shown: GoldHEN on FW 11.00 answers -1 to every form of the SDK's
 * version query, so the app only reports whether GoldHEN gave it root access. */

int jb_built_with_goldhen(void)
{
#ifdef USE_GOLDHEN
    return 1;
#else
    return 0;
#endif
}
