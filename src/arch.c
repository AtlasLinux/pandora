/* arch - simple .pnd archive packer/unpacker
 *
 * Usage:
 *   ./arch pack archive.pnd path1 [path2 ...]
 *   ./arch unpack archive.pnd [destdir]
 *
 * Notes:
 * - Stores regular files and symlinks.
 * - Paths inside archive are stored relative to the provided path arguments.
 * - Archive layout:
 *   [8 bytes magic "PNDARCH\1"]
 *   [u64 little-endian: entry_count]
 *   entries:
 *     [u32 path_len] [u64 file_size] [u64 file_offset] [u32 flags] [path bytes...]
 *   followed by data blobs concatenated.
 *
 * Flags:
 *   0x1 = symlink (blob contains the link target bytes)
 */

#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>

#define MAGIC "PNDARCH\1"
#define MAGIC_LEN 8
#define ENTRY_HDR_SIZE (4 + 8 + 8 + 4) /* u32 path_len, u64 size, u64 offset, u32 flags */

struct file_rec {
    char *path;        /* relative path stored in archive */
    char *src;         /* absolute source path on disk to read from */
    uint64_t size;     /* file size or symlink target len */
    uint64_t offset;   /* computed offset in archive blobs */
    uint32_t flags;    /* bit flags (0x1=symlink) */
};

static struct file_rec *g_recs = NULL;
static size_t g_rec_cap = 0;
static size_t g_rec_cnt = 0;
static size_t g_base_len = 0;
static char *g_basepath = NULL;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *r = strdup(s);
    if (!r) die("strdup failed");
    return r;
}

static bool path_is_absolute(const char *p) {
    return p[0] == '/';
}

/* create absolute path: if p is absolute return strdup(p), else cwd + "/" + p */
static char *make_abs_path(const char *p) {
    if (path_is_absolute(p)) return xstrdup(p);
    char *cwd = getcwd(NULL, 0);
    if (!cwd) die("getcwd failed: %s", strerror(errno));
    size_t need = strlen(cwd) + 1 + strlen(p) + 1;
    char *out = xmalloc(need);
    snprintf(out, need, "%s/%s", cwd, p);
    free(cwd);
    return out;
}

static void rec_append(const char *relpath, uint64_t size, uint32_t flags, const char *srcpath) {
    if (g_rec_cnt + 1 >= g_rec_cap) {
        g_rec_cap = g_rec_cap ? g_rec_cap * 2 : 256;
        g_recs = realloc(g_recs, g_rec_cap * sizeof(*g_recs));
        if (!g_recs) die("realloc failed");
    }
    g_recs[g_rec_cnt].path = xstrdup(relpath);
    g_recs[g_rec_cnt].src = xstrdup(srcpath);
    g_recs[g_rec_cnt].size = size;
    g_recs[g_rec_cnt].offset = 0;
    g_recs[g_rec_cnt].flags = flags;
    g_rec_cnt++;
}

/* callback used by nftw to collect files */
static int walk_cb(const char *fpath, const struct stat *sb,
                   int typeflag, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (!g_basepath) return 0;
    /* compute path relative to base */
    if (strncmp(fpath, g_basepath, g_base_len) != 0) {
        /* shouldn't happen */
        return 0;
    }
    const char *rel = fpath + g_base_len;
    if (*rel == '/') rel++; /* strip leading slash */
    if (*rel == '\0') return 0; /* skip base directory itself */

    /* compute absolute source path robustly */
    char *src_abs = make_abs_path(fpath);

    if (typeflag == FTW_F) {
        /* regular file */
        rec_append(rel, (uint64_t)sb->st_size, 0, src_abs);
    } else if (typeflag == FTW_SL) {
        /* symlink; read link target to know size */
        char buf[4096];
        ssize_t r = readlink(src_abs, buf, sizeof(buf));
        if (r < 0) die("readlink '%s': %s", src_abs, strerror(errno));
        rec_append(rel, (uint64_t)r, 0x1, src_abs);
    } else {
        /* skip directories, fifos, devices, etc. */
    }
    free(src_abs);
    return 0;
}

/* add single path (file or directory) to the file list */
static void add_path_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) die("lstat '%s': %s", path, strerror(errno));

    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        /* For a single file or symlink: compute absolute src and store basename as archive path */
        char *src_abs = make_abs_path(path);
        const char *slash = strrchr(path, '/');
        const char *rel = slash ? slash + 1 : path;
        if (S_ISREG(st.st_mode)) {
            rec_append(rel, (uint64_t)st.st_size, 0, src_abs);
        } else {
            char buf[4096];
            ssize_t r = readlink(src_abs, buf, sizeof(buf));
            if (r < 0) die("readlink '%s': %s", src_abs, strerror(errno));
            rec_append(rel, (uint64_t)r, 0x1, src_abs);
        }
        free(src_abs);
        return;
    }

    /* For directories: use realpath as base so stored paths are relative to it. */
    g_basepath = realpath(path, NULL);
    if (!g_basepath) die("realpath '%s': %s", path, strerror(errno));
    g_base_len = strlen(g_basepath);

    /* NOTE: pass g_basepath (absolute) to nftw so fpath values are absolute and
       the subsequent strncmp(fpath, g_basepath, g_base_len) works as intended. */
    if (nftw(g_basepath, walk_cb, 20, FTW_PHYS) != 0) die("nftw failed on '%s'", g_basepath);

    free(g_basepath);
    g_basepath = NULL;
}

/* write little-endian integer helpers */
static void write_u32_le(FILE *f, uint32_t v) {
    unsigned char b[4];
    b[0] = v & 0xff;
    b[1] = (v >> 8) & 0xff;
    b[2] = (v >> 16) & 0xff;
    b[3] = (v >> 24) & 0xff;
    if (fwrite(b, 1, 4, f) != 4) die("write failed");
}
static void write_u64_le(FILE *f, uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = (v >> (8*i)) & 0xff;
    if (fwrite(b, 1, 8, f) != 8) die("write failed");
}
static uint32_t read_u32_le(FILE *f) {
    unsigned char b[4];
    if (fread(b,1,4,f) != 4) die("unexpected EOF (u32)");
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t read_u64_le(FILE *f) {
    unsigned char b[8];
    if (fread(b,1,8,f) != 8) die("unexpected EOF (u64)");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= ((uint64_t)b[i]) << (8*i);
    return v;
}

/* copy file contents from absolute source path to archive stream, return number of bytes copied */
static uint64_t copy_file_to_stream(FILE *out, const char *srcpath) {
    FILE *in = fopen(srcpath, "rb");
    if (!in) die("fopen '%s': %s", srcpath, strerror(errno));
    uint64_t total = 0;
    char buf[65536];
    size_t r;
    while ((r = fread(buf,1,sizeof(buf), in)) > 0) {
        if (fwrite(buf,1,r,out) != r) die("write failed");
        total += r;
    }
    if (ferror(in)) die("read error on '%s'", srcpath);
    fclose(in);
    return total;
}

/* pack command implementation */
static void do_pack(int argc, char **argv) {
    if (argc < 3) die("pack requires: pack <archive.pnd> <file-or-dir>...");
    const char *arcname = argv[1];

    /* collect files */
    for (int i = 2; i < argc; ++i) {
        add_path_recursive(argv[i]);
    }
    if (g_rec_cnt == 0) die("no files collected");

    /* compute table size and offsets */
    uint64_t entry_count = (uint64_t)g_rec_cnt;
    uint64_t table_size = 0;
    for (size_t i = 0; i < g_rec_cnt; ++i) {
        uint32_t path_len = (uint32_t)strlen(g_recs[i].path);
        table_size += ENTRY_HDR_SIZE + path_len;
    }

    uint64_t header_size = MAGIC_LEN + 8; /* magic + entry_count u64 */
    uint64_t blob_start = header_size + table_size;

    uint64_t cur_offset = blob_start;
    for (size_t i = 0; i < g_rec_cnt; ++i) {
        g_recs[i].offset = cur_offset;
        cur_offset += g_recs[i].size;
    }

    /* open archive file (atomic write recommended by caller) */
    FILE *out = fopen(arcname, "wb");
    if (!out) die("fopen '%s' for write: %s", arcname, strerror(errno));

    /* write magic */
    if (fwrite(MAGIC, 1, MAGIC_LEN, out) != MAGIC_LEN) die("write magic failed");

    /* write entry_count (u64 le) */
    write_u64_le(out, entry_count);

    /* write table */
    for (size_t i = 0; i < g_rec_cnt; ++i) {
        uint32_t path_len = (uint32_t)strlen(g_recs[i].path);
        write_u32_le(out, path_len);
        write_u64_le(out, g_recs[i].size);
        write_u64_le(out, g_recs[i].offset);
        write_u32_le(out, g_recs[i].flags);
        if (fwrite(g_recs[i].path, 1, path_len, out) != path_len) die("write path failed");
    }

    /* write blobs in same order */
    for (size_t i = 0; i < g_rec_cnt; ++i) {
        if (g_recs[i].flags & 0x1) {
            /* symlink: read link target from absolute src and write bytes */
            char *src = g_recs[i].src;
            char *buf = malloc(g_recs[i].size + 1);
            if (!buf) die("malloc");
            ssize_t r = readlink(src, buf, g_recs[i].size + 1);
            if (r < 0) die("readlink '%s': %s", src, strerror(errno));
            if ((uint64_t)r != g_recs[i].size) {
                /* size changed between collect and pack; adjust but proceed */
            }
            if (fwrite(buf, 1, r, out) != (size_t)r) die("write symlink blob failed");
            free(buf);
        } else {
            /* regular file: copy from absolute src */
            uint64_t wrote = copy_file_to_stream(out, g_recs[i].src);
            if (wrote != g_recs[i].size) {
                fprintf(stderr, "warning: size changed while packing '%s' (expected %" PRIu64 ", wrote %" PRIu64 ")\n",
                        g_recs[i].path, g_recs[i].size, wrote);
            }
        }
    }

    if (fclose(out) != 0) die("fclose failed");
    printf("packed %zu entries into %s\n", g_rec_cnt, arcname);
}

/* ensure every parent directory for the given full path exists.
 * full_path is the full destination path including filename (e.g. /tmp/out/dir/file)
 */
static void ensure_parent_dirs(const char *full_path) {
    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s", full_path) >= sizeof(tmp))
        die("path too long in ensure_parent_dirs");

    /* find parent directory */
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return;            /* no parent component -> nothing to do */
    if (last_slash == tmp) return;     /* parent is root "/" -> nothing to do */

    *last_slash = '\0'; /* tmp now holds the parent directory */

    /* create each intermediate component */
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                die("mkdir '%s': %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    /* final parent */
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        die("mkdir '%s': %s", tmp, strerror(errno));
}

/* strip trailing slashes except keep single leading "/" */
static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n-1] == '/') s[--n] = '\0';
}

/* ensure destination directory exists and is a directory */
static void ensure_destdir(char *destbuf) {
    strip_trailing_slash(destbuf);

    struct stat st;
    if (stat(destbuf, &st) < 0) {
        if (errno == ENOENT) {
            if (mkdir(destbuf, 0755) < 0 && errno != EEXIST)
                die("mkdir '%s': %s", destbuf, strerror(errno));
            return;
        } else {
            die("stat '%s': %s", destbuf, strerror(errno));
        }
    }
    if (!S_ISDIR(st.st_mode)) die("destination '%s' exists and is not a directory", destbuf);
}

/* sanitize a stored archive relative path:
 * - remove leading slashes and "./"
 * - remove "." components
 * - resolve ".." by popping previous component (reject if escapes above root)
 * Returns malloc'd string or NULL if invalid/empty.
 */
static char *sanitize_relpath(const char *p)
{
    if (!p) return NULL;
    const char *cur = p;
    char comp[PATH_MAX];

    /* components stored temporarily */
    char *parts[PATH_MAX / 2];
    size_t part_len[PATH_MAX / 2];
    size_t nparts = 0;

    /* skip leading slashes and a single leading "./" */
    while (*cur == '/') cur++;
    if (cur[0] == '.' && cur[1] == '/') cur += 2;
    while (*cur == '/') cur++;

    while (*cur) {
        size_t i = 0;
        while (*cur != '/' && *cur != '\0') {
            if (i + 1 >= sizeof(comp)) { /* component too long */
                for (size_t j = 0; j < nparts; ++j) free(parts[j]);
                return NULL;
            }
            comp[i++] = *cur++;
        }
        comp[i] = '\0';
        while (*cur == '/') cur++; /* collapse slashes */

        if (i == 0) continue;
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (nparts == 0) {
                /* escapes above archive root — reject */
                for (size_t j = 0; j < nparts; ++j) free(parts[j]);
                return NULL;
            }
            free(parts[--nparts]);
            continue;
        }
        parts[nparts] = strdup(comp);
        if (!parts[nparts]) {
            for (size_t j = 0; j < nparts; ++j) free(parts[j]);
            die("out of memory");
        }
        part_len[nparts] = i;
        nparts++;
        if (nparts >= (sizeof(parts)/sizeof(parts[0]))) {
            for (size_t j = 0; j < nparts; ++j) free(parts[j]);
            die("path too deep");
        }
    }

    if (nparts == 0) {
        /* nothing meaningful */
        return NULL;
    }

    size_t tot = 0;
    for (size_t i = 0; i < nparts; ++i) tot += part_len[i] + (i ? 1 : 0);
    if (tot == 0 || tot >= PATH_MAX) {
        for (size_t j = 0; j < nparts; ++j) free(parts[j]);
        return NULL;
    }

    char *out = malloc(tot + 1);
    if (!out) {
        for (size_t j = 0; j < nparts; ++j) free(parts[j]);
        die("out of memory");
    }
    char *w = out;
    for (size_t i = 0; i < nparts; ++i) {
        if (i) *w++ = '/';
        memcpy(w, parts[i], part_len[i]);
        w += part_len[i];
        free(parts[i]);
    }
    *w = '\0';
    return out;
}

/* unpack: read table, compute blob_start, then stream blobs sequentially */
void do_unpack(int argc, char **argv) {
    if (argc < 2) die("unpack requires: unpack <archive.pnd> [destdir]");
    const char *arcname = argv[1];

    char destbuf[PATH_MAX];
    if (argc >= 3) {
        strncpy(destbuf, argv[2], sizeof(destbuf)-1);
        destbuf[sizeof(destbuf)-1] = '\0';
    } else {
        strcpy(destbuf, ".");
    }
    ensure_destdir(destbuf);
    const char *dest = destbuf;

    FILE *in = fopen(arcname, "rb");
    if (!in) die("fopen '%s' for read: %s", arcname, strerror(errno));

    char magic[MAGIC_LEN];
    if (fread(magic, 1, MAGIC_LEN, in) != MAGIC_LEN) die("read magic failed");
    if (memcmp(magic, MAGIC, MAGIC_LEN) != 0) die("bad magic - not a pnd archive");

    uint64_t entry_count = read_u64_le(in);
    if (entry_count == 0) {
        fprintf(stderr, "empty archive\n");
        fclose(in);
        return;
    }

    /* read table entries into recs[], but also compute table_size so we can find blob_start */
    uint64_t table_size = 0;
    struct file_rec *recs = calloc(entry_count, sizeof(*recs));
    if (!recs) die("calloc failed");

    for (uint64_t i = 0; i < entry_count; ++i) {
        uint32_t path_len = read_u32_le(in);
        uint64_t size = read_u64_le(in);
        uint64_t offset = read_u64_le(in); /* read but ignore later */
        uint32_t flags = read_u32_le(in);

        table_size += ENTRY_HDR_SIZE + (uint64_t)path_len;

        if (path_len == 0) {
            recs[i].path = NULL;
            recs[i].size = size;
            recs[i].offset = offset;
            recs[i].flags = flags;
            continue;
        }

        char *raw = malloc(path_len + 1);
        if (!raw) die("malloc");
        if (fread(raw, 1, path_len, in) != path_len) die("read path failed");
        raw[path_len] = '\0';

        char *san = sanitize_relpath(raw);
        free(raw);

        if (!san) {
            recs[i].path = NULL;
        } else {
            recs[i].path = san;
        }
        recs[i].size = size;
        recs[i].offset = offset;
        recs[i].flags = flags;
    }

    /* compute blob_start (should match pack) */
    uint64_t header_size = MAGIC_LEN + 8; /* magic + entry_count */
    uint64_t blob_start = header_size + table_size;

    /* open manifest safely */
    char manifest_path[PATH_MAX];
    if (strcmp(dest, "/") == 0) {
        if (snprintf(manifest_path, sizeof(manifest_path), "/.manifest") >= (int)sizeof(manifest_path))
            die("manifest path too long");
    } else {
        if (snprintf(manifest_path, sizeof(manifest_path), "%s/.manifest", dest) >= (int)sizeof(manifest_path))
            die("manifest path too long");
    }
    FILE *manifest = fopen(manifest_path, "w");
    if (!manifest) die("fopen manifest '%s': %s", manifest_path, strerror(errno));

    /* seek to blob_start and read blobs sequentially in table order */
    if (fseeko(in, (off_t)blob_start, SEEK_SET) != 0) die("fseeko failed to blob_start: %s", strerror(errno));

    for (uint64_t i = 0; i < entry_count; ++i) {
        if (!recs[i].path) {
            fprintf(stderr, "warning: skipping empty or invalid archive entry at index %" PRIu64 "\n", i);
            /* still must advance file pointer by recs[i].size to keep stream aligned */
            if (recs[i].size > 0) {
                if (fseeko(in, (off_t)recs[i].size, SEEK_CUR) != 0)
                    die("fseeko failed skipping blob: %s", strerror(errno));
            }
            continue;
        }

        char outpath[PATH_MAX];
        if (strcmp(dest, "/") == 0) {
            if (snprintf(outpath, sizeof(outpath), "/%s", recs[i].path) >= (int)sizeof(outpath))
                die("path too long for extraction");
        } else if (strcmp(dest, ".") == 0) {
            if (snprintf(outpath, sizeof(outpath), "%s", recs[i].path) >= (int)sizeof(outpath))
                die("path too long for extraction");
        } else {
            if (snprintf(outpath, sizeof(outpath), "%s/%s", dest, recs[i].path) >= (int)sizeof(outpath))
                die("path too long for extraction");
        }

        ensure_parent_dirs(outpath);

        if (recs[i].flags & 0x1) {
            /* symlink: read target bytes */
            char *buf = malloc(recs[i].size + 1);
            if (!buf) die("malloc");
            if (recs[i].size > 0) {
                if (fread(buf, 1, recs[i].size, in) != recs[i].size) die("read symlink target failed");
            }
            buf[recs[i].size] = '\0';
            unlink(outpath);
            if (symlink(buf, outpath) < 0)
                die("symlink '%s' -> '%s' failed: %s", outpath, buf, strerror(errno));
            free(buf);
        } else {
            /* regular file: stream bytes */
            FILE *out = fopen(outpath, "wb");
            if (!out) die("fopen '%s': %s", outpath, strerror(errno));
            uint64_t remaining = recs[i].size;
            char buf[65536];
            while (remaining > 0) {
                size_t toread = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
                if (fread(buf, 1, toread, in) != toread) die("read blob failed");
                if (fwrite(buf, 1, toread, out) != toread) die("write out failed");
                remaining -= toread;
            }
            if (fclose(out) != 0) die("fclose failed for '%s'", outpath);
        }

        if (fprintf(manifest, "%s\n", recs[i].path) < 0)
            die("write to manifest failed");

        printf("extracted: %s\n", outpath);
    }

    if (fclose(manifest) != 0)
        die("fclose manifest failed");

    for (uint64_t i = 0; i < entry_count; ++i) {
        if (recs[i].path) free(recs[i].path);
    }
    free(recs);
    fclose(in);
}

#ifndef NO_MAIN
/* main: simple CLI dispatch */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage:\n  %s pack <archive.pnd> <file-or-dir>...\n  %s unpack <archive.pnd> [destdir]\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "pack") == 0) {
        /* shift argv so pack sees argv[1]=archive */
        do_pack(argc - 1, argv + 1);
    } else if (strcmp(argv[1], "unpack") == 0) {
        do_unpack(argc - 1, argv + 1);
    } else {
        fprintf(stderr, "unknown command '%s'\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* free recorded src/path entries */
    for (size_t i = 0; i < g_rec_cnt; ++i) {
        free(g_recs[i].path);
        free(g_recs[i].src);
    }
    free(g_recs);

    return EXIT_SUCCESS;
}
#endif