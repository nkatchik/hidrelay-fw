#ifndef HIDRELAY_UTIL_CLEANUP_H
#define HIDRELAY_UTIL_CLEANUP_H

#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define UTIL_CLEANUP(function_name) __attribute__((cleanup(function_name)))
#else
#error "Compiler must support __attribute__((cleanup(...)))."
#endif

#define UTIL_SCOPED_FREE UTIL_CLEANUP(util_cleanup_freep)
#define UTIL_SCOPED_FILE UTIL_CLEANUP(util_cleanup_filep)
#define UTIL_SCOPED_FD UTIL_CLEANUP(util_cleanup_fdp)

void util_cleanup_freep(void * pointer_location);
void util_cleanup_filep(FILE ** file_pointer);
void util_cleanup_fdp(int * fd_pointer);

#endif
