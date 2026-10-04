#ifndef MONITORS_H
#define MONITORS_H

#include "geometry.h"

/* Enumerate currently connected displays into `out` (capacity `max`).
 * Returns the number written. Requires SDL. A missing/headless display
 * yields zero monitors; callers then keep the window at a safe virtual
 * position until a display returns. */
int monitors_enumerate(Monitor *out, int max);

#endif /* MONITORS_H */
