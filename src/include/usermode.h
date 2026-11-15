#ifndef USERMODE_H
#define USERMODE_H

void usermode_launch(const char *filename);

/* Called after a ring-3 process exits or faults. */
void usermode_cleanup_current(void);
void usermode_reap_deferred(void);

#endif
