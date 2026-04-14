/*
 * DVD CSS protocol using libdvdcss
 *
 * URL syntax:
 *   dvdcss://D:/title1
 *   dvdcss://C:/path/to/VIDEO_TS/title2
 *
 * Behavior:
 *   - Requires explicit /titleN
 *   - No auto main-title
 *   - On invalid title, prints available titles with sizes
 */

#include "config.h"

#if CONFIG_DVDCSS_PROTOCOL

#include <dvdcss/dvdcss.h>

#include "libavformat/avio.h"
#include "libavformat/url.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

typedef struct DVDTitleEntry {
    int     title_number;   /* 1-based */
    char  **vob_paths;      /* NULL-terminated array of VOB file paths */
    int     nb_vobs;
    int64_t total_size;     /* bytes */
} DVDTitleEntry;

typedef struct DVDTitleList {
    DVDTitleEntry *titles;
    int            nb_titles;
} DVDTitleList;

typedef struct DVDVOBPos {
    int     title_index;    /* index into DVDTitleList.titles */
    int     vob_index;      /* index into vob_paths */
    int64_t offset_in_vob;  /* byte offset within current VOB */
} DVDVOBPos;

typedef struct DVDContext {
    const AVClass *class;

    char          *root_path;     /* path to drive or VIDEO_TS root */
    int            title_number;  /* requested title (1-based) */

    dvdcss_t       css;

    DVDTitleList   title_list;
    int            current_title_index;
    DVDTitleEntry *current_title;

    DVDVOBPos      pos;
    int64_t        logical_pos;   /* byte offset within title */
    int64_t        logical_size;  /* total bytes in title */

    unsigned char *sector_buf;
    int            sector_buf_valid;
    int64_t        sector_buf_lba; /* LBA of buffered sector */

    int            sector_size;
} DVDContext;

#define OFFSET(x) offsetof(DVDContext, x)
#define E AV_OPT_FLAG_DECODING_PARAM

static const AVOption dvdcss_options[] = {
    { "title", "DVD title number (1-based)", OFFSET(title_number), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, E },
    { NULL }
};

static const AVClass dvdcss_class = {
    .class_name = "dvdcss protocol",
    .item_name  = av_default_item_name,
    .option     = dvdcss_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* --- Utility helpers --------------------------------------------------- */

static int is_vob_file(const char *name)
{
    size_t len = strlen(name);
    if (len < 8)
        return 0;
    if (av_strncasecmp(name, "VTS_", 4))
        return 0;
    if (av_strcasecmp(name + len - 4, ".VOB"))
        return 0;
    return 1;
}

/* Extract title number from VTS_XX_Y.VOB (XX) */
static int vob_title_from_name(const char *name)
{
    int t = 0;
    if (sscanf(name, "VTS_%2d_", &t) == 1 && t > 0)
        return t;
    return 0;
}

/* Compare VOB names by their _Y index (VTS_XX_Y.VOB) */
static int vob_index_from_name(const char *name)
{
    int t = 0, idx = 0;
    if (sscanf(name, "VTS_%2d_%d.VOB", &t, &idx) == 2 && idx > 0)
        return idx;
    return 0;
}

static int64_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    return st.st_size;
}

/* Join root + name into a full path */
static char *join_path(const char *root, const char *name)
{
    size_t len_root = strlen(root);
    size_t len_name = strlen(name);
    int need_sep = (len_root > 0 && root[len_root - 1] != '/' && root[len_root - 1] != '\\');
    char *res = av_malloc(len_root + need_sep + len_name + 1);
    if (!res)
        return NULL;
    memcpy(res, root, len_root);
    if (need_sep)
        res[len_root++] = '/';
    memcpy(res + len_root, name, len_name);
    res[len_root + len_name] = '\0';
    return res;
}

/* --- Title scanning ---------------------------------------------------- */

static void free_title_list(DVDTitleList *list)
{
    int i, j;
    if (!list)
        return;
    for (i = 0; i < list->nb_titles; i++) {
        DVDTitleEntry *te = &list->titles[i];
        if (te->vob_paths) {
            for (j = 0; j < te->nb_vobs; j++)
                av_freep(&te->vob_paths[j]);
            av_freep(&te->vob_paths);
        }
    }
    av_freep(&list->titles);
    list->nb_titles = 0;
}

/* Scan VIDEO_TS-like directory and build title list */
static int scan_titles(AVFormatContext *s, DVDContext *dvd)
{
    DVDTitleList list = { 0 };
    DIR *dir;
    struct dirent *de;
    int ret = 0;
    int max_titles = 0;

    dir = opendir(dvd->root_path);
    if (!dir) {
        av_log(s, AV_LOG_ERROR, "dvdcss: cannot open directory '%s'\n", dvd->root_path);
        return AVERROR(errno);
    }

    /* First pass: find max title number */
    while ((de = readdir(dir))) {
        if (!is_vob_file(de->d_name))
            continue;
        int t = vob_title_from_name(de->d_name);
        if (t > max_titles)
            max_titles = t;
    }

    if (max_titles <= 0) {
        av_log(s, AV_LOG_ERROR, "dvdcss: no VOB files found in '%s'\n", dvd->root_path);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    list.titles = av_calloc(max_titles, sizeof(DVDTitleEntry));
    if (!list.titles) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    list.nb_titles = max_titles;

    /* Initialize title numbers */
    for (int i = 0; i < max_titles; i++)
        list.titles[i].title_number = i + 1;

    /* Second pass: collect VOBs per title */
    rewinddir(dir);
    while ((de = readdir(dir))) {
        char *full;
        int t, idx;
        int64_t sz;

        if (!is_vob_file(de->d_name))
            continue;

        t = vob_title_from_name(de->d_name);
        if (t <= 0 || t > max_titles)
            continue;

        idx = vob_index_from_name(de->d_name);
        if (idx <= 0)
            continue;

        full = join_path(dvd->root_path, de->d_name);
        if (!full) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        sz = file_size(full);
        if (sz < 0) {
            av_log(s, AV_LOG_WARNING, "dvdcss: cannot stat '%s'\n", full);
            av_free(full);
            continue;
        }

        DVDTitleEntry *te = &list.titles[t - 1];

        /* grow vob_paths array */
        char **new_vobs = av_realloc_array(te->vob_paths, te->nb_vobs + 1, sizeof(char *));
        if (!new_vobs) {
            av_free(full);
            ret = AVERROR(ENOMEM);
            goto end;
        }
        te->vob_paths = new_vobs;
        te->vob_paths[te->nb_vobs] = full;
        te->nb_vobs++;
        te->total_size += sz;
    }

    /* Sort VOBs within each title by index */
    for (int i = 0; i < list.nb_titles; i++) {
        DVDTitleEntry *te = &list.titles[i];
        if (te->nb_vobs <= 1)
            continue;

        for (int a = 0; a < te->nb_vobs - 1; a++) {
            for (int b = a + 1; b < te->nb_vobs; b++) {
                int idx_a = vob_index_from_name(strrchr(te->vob_paths[a], '/') ?
                                                strrchr(te->vob_paths[a], '/') + 1 :
                                                te->vob_paths[a]);
                int idx_b = vob_index_from_name(strrchr(te->vob_paths[b], '/') ?
                                                strrchr(te->vob_paths[b], '/') + 1 :
                                                te->vob_paths[b]);
                if (idx_b < idx_a) {
                    char *tmp = te->vob_paths[a];
                    te->vob_paths[a] = te->vob_paths[b];
                    te->vob_paths[b] = tmp;
                }
            }
        }
    }

    dvd->title_list = list;
    list.titles = NULL; /* ownership moved */
    ret = 0;

end:
    if (dir)
        closedir(dir);
    if (list.titles)
        free_title_list(&list);
    return ret;
}

/* Print available titles with sizes */
static void log_available_titles(AVFormatContext *s, DVDContext *dvd)
{
    av_log(s, AV_LOG_INFO, "dvdcss: Available titles:\n");
    for (int i = 0; i < dvd->title_list.nb_titles; i++) {
        DVDTitleEntry *te = &dvd->title_list.titles[i];
        if (te->nb_vobs == 0)
            continue;
        av_log(s, AV_LOG_INFO, "  title%d: %"PRId64" bytes (%d VOBs)\n",
               te->title_number, te->total_size, te->nb_vobs);
    }
}

/* --- Position helpers -------------------------------------------------- */

static int64_t title_total_size(DVDTitleEntry *te)
{
    return te ? te->total_size : 0;
}

/* Map logical byte offset within title to VOB index + offset */
static int map_logical_to_vob(DVDTitleEntry *te, int64_t logical, int *vob_index, int64_t *offset_in_vob)
{
    int64_t acc = 0;

    for (int i = 0; i < te->nb_vobs; i++) {
        int64_t sz = file_size(te->vob_paths[i]);
        if (sz < 0)
            return AVERROR(EIO);
        if (logical < acc + sz) {
            *vob_index = i;
            *offset_in_vob = logical - acc;
            return 0;
        }
        acc += sz;
    }

    return AVERROR_EOF;
}

/* --- URL parsing ------------------------------------------------------- */

static int parse_dvdcss_url(AVFormatContext *s, DVDContext *dvd, const char *url)
{
    const char *p = url;
    char *path = NULL;
    char *title_str = NULL;
    int title = 0;
    int ret = 0;

    if (!av_strstart(p, "dvdcss://", &p)) {
        av_log(s, AV_LOG_ERROR, "dvdcss: invalid URL, must start with dvdcss://\n");
        return AVERROR(EINVAL);
    }

    /* Everything up to /titleN is root path */
    const char *title_pos = strstr(p, "/title");
    if (!title_pos) {
        av_log(s, AV_LOG_ERROR, "dvdcss: no title specified, use dvdcss://<path>/titleN\n");
        return AVERROR(EINVAL);
    }

    path = av_strndup(p, title_pos - p);
    if (!path)
        return AVERROR(ENOMEM);

    title_str = av_strdup(title_pos + strlen("/title"));
    if (!title_str) {
        av_free(path);
        return AVERROR(ENOMEM);
    }

    title = strtol(title_str, NULL, 10);
    if (title <= 0) {
        av_log(s, AV_LOG_ERROR, "dvdcss: invalid title '%s'\n", title_str);
        av_free(path);
        av_free(title_str);
        return AVERROR(EINVAL);
    }

    dvd->root_path = path;
    dvd->title_number = title;

    av_free(title_str);
    return ret;
}

/* --- libdvdcss integration --------------------------------------------- */

/*
 * For simplicity, we open each VOB file via libdvdcss in "file mode".
 * For real discs, you might want to open the device instead and map LBAs.
 */

static int dvdcss_open_vob(AVFormatContext *s, DVDContext *dvd, const char *path)
{
    if (dvd->css) {
        dvdcss_close(dvd->css);
        dvd->css = NULL;
    }

    dvd->css = dvdcss_open(path);
    if (!dvd->css) {
        av_log(s, AV_LOG_ERROR, "dvdcss: failed to open '%s'\n", path);
        return AVERROR(EIO);
    }

    dvd->sector_size = 2048;
    dvd->sector_buf_valid = 0;
    dvd->sector_buf_lba = -1;

    return 0;
}

/* Read bytes from current title at logical_pos into buf */
static int dvdcss_read_bytes(AVFormatContext *s, DVDContext *dvd, unsigned char *buf, int size)
{
    int64_t pos = dvd->logical_pos;
    int total_read = 0;
    int ret;

    while (size > 0) {
        int vob_index;
        int64_t offset_in_vob;
        int64_t vob_size;
        int64_t remaining_in_vob;
        int to_read;

        if (pos >= dvd->logical_size)
            break;

        ret = map_logical_to_vob(dvd->current_title, pos, &vob_index, &offset_in_vob);
        if (ret < 0)
            break;

        if (vob_index != dvd->pos.vob_index || dvd->css == NULL) {
            /* open new VOB */
            ret = dvdcss_open_vob(s, dvd, dvd->current_title->vob_paths[vob_index]);
            if (ret < 0)
                return ret;
            dvd->pos.vob_index = vob_index;
        }

        vob_size = file_size(dvd->current_title->vob_paths[vob_index]);
        if (vob_size < 0)
            return AVERROR(EIO);

        remaining_in_vob = vob_size - offset_in_vob;
        if (remaining_in_vob <= 0) {
            pos += remaining_in_vob;
            continue;
        }

        to_read = FFMIN(size, remaining_in_vob);

        /* libdvdcss works in sectors; we simplify by seeking via fseek-like behavior */
        {
            int lba = offset_in_vob / dvd->sector_size;
            int sector_offset = offset_in_vob % dvd->sector_size;
            int sectors_needed = (sector_offset + to_read + dvd->sector_size - 1) / dvd->sector_size;
            int got = 0;
            unsigned char *tmp = buf;

            /* Seek to LBA */
            if (dvdcss_seek(dvd->css, lba, DVDCSS_SEEK_SET) < 0)
                return AVERROR(EIO);

            for (int i = 0; i < sectors_needed; i++) {
                int r = dvdcss_read(dvd->css, dvd->sector_buf, 1, -1);
                if (r <= 0)
                    return AVERROR(EIO);

                int copy_offset = (i == 0) ? sector_offset : 0;
                int copy_size = dvd->sector_size - copy_offset;
                if (copy_size > to_read - got)
                    copy_size = to_read - got;

                memcpy(tmp + got, dvd->sector_buf + copy_offset, copy_size);
                got += copy_size;
                if (got >= to_read)
                    break;
            }

            to_read = got;
        }

        buf += to_read;
        size -= to_read;
        pos += to_read;
        total_read += to_read;
        dvd->logical_pos = pos;
    }

    return total_read ? total_read : AVERROR_EOF;
}

/* --- Protocol callbacks ------------------------------------------------ */

static int dvdcss_url_open(URLContext *h, const char *url, int flags)
{
    AVFormatContext *s = h->priv_data;
    DVDContext *dvd = s->priv_data;
    int ret;

    dvd->sector_size = 2048;
    dvd->sector_buf = av_malloc(dvd->sector_size);
    if (!dvd->sector_buf)
        return AVERROR(ENOMEM);

    ret = parse_dvdcss_url(s, dvd, url);
    if (ret < 0)
        return ret;

    /* Scan titles in root_path (must be VIDEO_TS-like) */
    ret = scan_titles(s, dvd);
    if (ret < 0)
        return ret;

    /* Find requested title */
    dvd->current_title_index = -1;
    for (int i = 0; i < dvd->title_list.nb_titles; i++) {
        if (dvd->title_list.titles[i].title_number == dvd->title_number &&
            dvd->title_list.titles[i].nb_vobs > 0) {
            dvd->current_title_index = i;
            dvd->current_title = &dvd->title_list.titles[i];
            break;
        }
    }

    if (dvd->current_title_index < 0 || !dvd->current_title) {
        av_log(s, AV_LOG_ERROR, "dvdcss: title %d not found on '%s'\n",
               dvd->title_number, dvd->root_path);
        log_available_titles(s, dvd);
        return AVERROR_INVALIDDATA;
    }

    dvd->logical_size = title_total_size(dvd->current_title);
    dvd->logical_pos  = 0;
    dvd->pos.vob_index = -1;
    dvd->pos.offset_in_vob = 0;

    av_log(s, AV_LOG_INFO, "dvdcss: using title %d (%"PRId64" bytes, %d VOBs)\n",
           dvd->title_number, dvd->logical_size, dvd->current_title->nb_vobs);

    h->is_streamed = 0;
    h->is_connected = 1;

    return 0;
}

static int dvdcss_url_read(URLContext *h, unsigned char *buf, int size)
{
    AVFormatContext *s = h->priv_data;
    DVDContext *dvd = s->priv_data;

    if (size <= 0)
        return 0;

    return dvdcss_read_bytes(s, dvd, buf, size);
}

static int64_t dvdcss_url_seek(URLContext *h, int64_t pos, int whence)
{
    AVFormatContext *s = h->priv_data;
    DVDContext *dvd = s->priv_data;
    int64_t new_pos;

    switch (whence) {
    case AVSEEK_SIZE:
        return dvd->logical_size;
    case SEEK_SET:
        new_pos = pos;
        break;
    case SEEK_CUR:
        new_pos = dvd->logical_pos + pos;
        break;
    case SEEK_END:
        new_pos = dvd->logical_size + pos;
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (new_pos < 0 || new_pos > dvd->logical_size)
        return AVERROR(EINVAL);

    dvd->logical_pos = new_pos;
    dvd->pos.vob_index = -1;
    dvd->pos.offset_in_vob = 0;

    return new_pos;
}

static int dvdcss_url_close(URLContext *h)
{
    AVFormatContext *s = h->priv_data;
    DVDContext *dvd = s->priv_data;

    if (dvd->css) {
        dvdcss_close(dvd->css);
        dvd->css = NULL;
    }

    if (dvd->sector_buf) {
        av_freep(&dvd->sector_buf);
    }

    if (dvd->root_path) {
        av_freep(&dvd->root_path);
    }

    free_title_list(&dvd->title_list);

    return 0;
}

const URLProtocol ff_dvdcss_protocol = {
    .name           = "dvdcss",
    .url_open       = dvdcss_url_open,
    .url_read       = dvdcss_url_read,
    .url_seek       = dvdcss_url_seek,
    .url_close      = dvdcss_url_close,
    .priv_data_size = sizeof(DVDContext),
    .priv_data_class= &dvdcss_class,
};

#endif /* CONFIG_DVDCSS_PROTOCOL */
