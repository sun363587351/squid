#ifndef _INC_INET_PTON_H
#define _INC_INET_PTON_H

#include "config.h"

#if HAVE_INET_PTON

/* Use the system provided version where possible */
#define xinet_pton inet_pton

#else

/* int
 * inet_pton(af, src, dst)
 *      convert from presentation format (which usually means ASCII printable)
 *      to network format (which is usually some kind of binary format).
 * return:
 *      1 if the address was valid for the specified address family
 *      0 if the address wasn't valid (`dst' is untouched in this case)
 *      -1 if some other error occurred (`dst' is untouched in this case, too)
 * author:
 *      Paul Vixie, 1996.
 */
SQUIDCEXTERN int xinet_pton(int af, const char *src, void *dst);

#endif

#endif /* _INC_INET_NTOP_H */
