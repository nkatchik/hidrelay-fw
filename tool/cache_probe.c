#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util/cleanup.h"

enum {
    CACHE_PROBE_CHUNK_SIZE = 1024
};

static int cache_probe_count_bytes(
    const char * path,
    size_t * out_size
) {
    UTIL_SCOPED_FILE FILE * file = NULL;
    UTIL_SCOPED_FREE unsigned char * buffer = NULL;
    size_t total_size = 0U;

    if ((path == NULL) || (out_size == NULL)) {
        return -1;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    buffer = calloc(CACHE_PROBE_CHUNK_SIZE, sizeof(*buffer));
    if (buffer == NULL) {
        return -1;
    }

    for (;;) {
        const size_t read_bytes = fread(buffer, 1U, CACHE_PROBE_CHUNK_SIZE, file);
        total_size += read_bytes;

        if (read_bytes < CACHE_PROBE_CHUNK_SIZE) {
            if (ferror(file) != 0) {
                return -1;
            }

            break;
        }
    }

    *out_size = total_size;
    return 0;
}

int main(
    int argc,
    char ** argv
) {
    UTIL_SCOPED_FD int file_descriptor = -1;
    size_t file_size = 0U;

    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s <path>\n", argv[0]);
        return 2;
    }

    file_descriptor = open(argv[1], O_RDONLY);
    if (file_descriptor < 0) {
        (void)fprintf(stderr, "open failed for %s: %d\n", argv[1], errno);
        return 1;
    }

    if (cache_probe_count_bytes(argv[1], &file_size) != 0) {
        (void)fprintf(stderr, "read failed for %s\n", argv[1]);
        return 1;
    }

    (void)printf("%s: %zu bytes\n", argv[1], file_size);
    return 0;
}
