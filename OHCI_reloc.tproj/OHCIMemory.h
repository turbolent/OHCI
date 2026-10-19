#ifndef OHCI_MEMORY_H
#define OHCI_MEMORY_H

#include <string.h>
#ifdef OHCI_HOST_TEST
/* Host tests use standard C equivalents of the BSD memory routines. */
#define bzero(address, length) memset((address), 0, (length))
#define bcopy(source, destination, length) memmove((destination), (source), (length))
#endif

#endif
