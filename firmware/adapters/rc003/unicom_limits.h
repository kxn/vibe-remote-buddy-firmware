#ifndef RBP_UNICOM_LIMITS_H
#define RBP_UNICOM_LIMITS_H

/* Maximum consecutive 20 ms ICO units accepted as recoverable loss (1 s).
 * This is a transport guard, not a limit imposed by the concealment routine. */
#define UNICOM_ICO_MAX_CONCEALED_UNITS 50u

#endif
