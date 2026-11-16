#include "sfs2.h"

#include <stdio.h>

int main(void) {
    if (!sfs2_ci_metadata_self_test()) {
        fputs("SFS2 metadata self-test FAILED\\n", stderr);
        return 1;
    }
    puts("SFS2 metadata self-test passed");

    if (!sfs2_ci_mount_read_test()) {
        fputs("SFS2 mount/journal/filesystem self-test FAILED\\n", stderr);
        return 1;
    }
    puts("SFS2 mount, journal recovery, nested writes, fault injection, and large-file self-test passed");
    return 0;
}
