#ifndef _jack_sysdep_sanitycheck_c_
#define _jack_sysdep_sanitycheck_c_

#if defined(__gnu_linux__)
#include <config/os/gnu-linux/sanitycheck.c>
#elif defined(__MACH__) && defined(__APPLE__)
#include <config/os/macosx/sanitycheck.c>
#else
#include <config/os/generic/sanitycheck.c>
#endif

#endif /* _jack_sysdep_sanitycheck_c_ */
