#include "util/cleanup.h"

#include <stdlib.h>
#include <unistd.h>

void util_cleanup_freep(void *pointer_location) {
    void **typed_pointer = (void **)pointer_location;

    if (typed_pointer == NULL) {
        return;
    }

    if (*typed_pointer == NULL) {
        return;
    }

    free(*typed_pointer);
    *typed_pointer = NULL;
}

void util_cleanup_filep(FILE **file_pointer) {
    if (file_pointer == NULL) {
        return;
    }

    if (*file_pointer == NULL) {
        return;
    }

    (void)fclose(*file_pointer);
    *file_pointer = NULL;
}

void util_cleanup_fdp(int *fd_pointer) {
    if (fd_pointer == NULL) {
        return;
    }

    if (*fd_pointer < 0) {
        return;
    }

    (void)close(*fd_pointer);
    *fd_pointer = -1;
}
