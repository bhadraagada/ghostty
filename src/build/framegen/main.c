#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define SEPARATOR '\x01'
#define CHUNK_SIZE 16384
#define MAX_FRAMES 1024

typedef struct {
    char *name;
} FrameEntry;

static int filter_txt(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".txt") == 0;
}

static int compare_frames(const void *a, const void *b) {
    return strcmp(((FrameEntry*)a)->name, ((FrameEntry*)b)->name);
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, size, f) != (size_t)size) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = size;
    return buf;
}

#ifdef _WIN32
static int scan_frames_dir(const char *frames_dir, FrameEntry **out_entries) {
    char search_path[4096];
    snprintf(search_path, sizeof(search_path), "%s\\*.txt", frames_dir);

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            *out_entries = NULL;
            return 0;
        }
        fprintf(stderr, "Failed to scan directory %s: error %lu\n", frames_dir, err);
        return -1;
    }

    FrameEntry *entries = malloc(MAX_FRAMES * sizeof(FrameEntry));
    int count = 0;

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (filter_txt(find_data.cFileName) && count < MAX_FRAMES) {
                entries[count].name = _strdup(find_data.cFileName);
                count++;
            }
        }
    } while (FindNextFileA(hFind, &find_data) != 0);

    FindClose(hFind);

    if (count > 0) {
        qsort(entries, count, sizeof(FrameEntry), compare_frames);
    }

    *out_entries = entries;
    return count;
}
#else
static int filter_frames(const struct dirent *entry) {
    return filter_txt(entry->d_name);
}

static int compare_dirent(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int scan_frames_dir(const char *frames_dir, FrameEntry **out_entries) {
    struct dirent **namelist;
    int n = scandir(frames_dir, &namelist, filter_frames, compare_dirent);
    if (n < 0) {
        fprintf(stderr, "Failed to scan directory %s: %s\n", frames_dir, strerror(errno));
        return -1;
    }

    FrameEntry *entries = malloc(n * sizeof(FrameEntry));
    for (int i = 0; i < n; i++) {
        entries[i].name = strdup(namelist[i]->d_name);
        free(namelist[i]);
    }
    free(namelist);

    *out_entries = entries;
    return n;
}
#endif

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <frames_dir> <output_file>\n", argv[0]);
        return 1;
    }

    const char *frames_dir = argv[1];
    const char *output_file = argv[2];

    FrameEntry *entries;
    int n = scan_frames_dir(frames_dir, &entries);
    if (n < 0) {
        return 1;
    }

    if (n == 0) {
        fprintf(stderr, "No frame files found in %s\n", frames_dir);
        return 1;
    }

    size_t total_size = 0;
    char **frame_contents = calloc(n, sizeof(char*));
    size_t *frame_sizes = calloc(n, sizeof(size_t));

    for (int i = 0; i < n; i++) {
        char path[4096];
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s\\%s", frames_dir, entries[i].name);
#else
        snprintf(path, sizeof(path), "%s/%s", frames_dir, entries[i].name);
#endif
        
        frame_contents[i] = read_file(path, &frame_sizes[i]);
        if (!frame_contents[i]) {
            return 1;
        }
        
        total_size += frame_sizes[i];
        if (i < n - 1) total_size++;
    }

    char *joined = malloc(total_size);
    if (!joined) {
        fprintf(stderr, "Failed to allocate joined buffer\n");
        return 1;
    }

    size_t offset = 0;
    for (int i = 0; i < n; i++) {
        memcpy(joined + offset, frame_contents[i], frame_sizes[i]);
        offset += frame_sizes[i];
        if (i < n - 1) {
            joined[offset++] = SEPARATOR;
        }
    }

    uLongf compressed_size = compressBound(total_size);
    unsigned char *compressed = malloc(compressed_size);
    if (!compressed) {
        fprintf(stderr, "Failed to allocate compression buffer\n");
        return 1;
    }

    z_stream stream = {0};
    stream.next_in = (unsigned char*)joined;
    stream.avail_in = total_size;
    stream.next_out = compressed;
    stream.avail_out = compressed_size;

    // Use -MAX_WBITS for raw DEFLATE (no zlib wrapper)
    int ret = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        fprintf(stderr, "deflateInit2 failed: %d\n", ret);
        return 1;
    }

    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        fprintf(stderr, "deflate failed: %d\n", ret);
        deflateEnd(&stream);
        return 1;
    }

    compressed_size = stream.total_out;
    deflateEnd(&stream);
    
    FILE *out = fopen(output_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create %s: %s\n", output_file, strerror(errno));
        return 1;
    }

    if (fwrite(compressed, 1, compressed_size, out) != compressed_size) {
        fprintf(stderr, "Failed to write compressed data\n");
        return 1;
    }

    fclose(out);

    // Cleanup
    for (int i = 0; i < n; i++) {
        free(frame_contents[i]);
        free(entries[i].name);
    }
    free(frame_contents);
    free(frame_sizes);
    free(entries);
    free(joined);
    free(compressed);

    return 0;
}
