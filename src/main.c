/* LVGL 9 verification app for TB-4117 over USB-NCM (ISAPI) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <setjmp.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#include <curl/curl.h>
#include <jpeglib.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "lvgl/lvgl.h"

#define MODULE_URL  "http://192.168.64.64"
#define AUTH        "admin:REDACTED"
#define IMG_W       320          /* source resolution of the module */
#define IMG_H       240
#define VIEW_W      480          /* what we hand to LVGL (pre-scaled) */
#define VIEW_H      360
#define PICTURE_URL MODULE_URL "/ISAPI/Streaming/channels/101/picture"
#define RTSP_URL    "rtsp://admin:REDACTED@192.168.64.64:554/Streaming/Channels/101"
#define TEMP_URL    MODULE_URL "/ISAPI/Thermal/channels/1/thermometry/1/rulesTemperatureInfo?format=json"

static uint8_t frame_tmp[VIEW_W * VIEW_H * 2];   /* worker decodes here */
static uint8_t frame_pending[VIEW_W * VIEW_H * 2]; /* handed to UI */
static uint8_t frame_show[VIEW_W * VIEW_H * 2];  /* what LVGL paints */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int frame_ready = 0;
static volatile int running = 1;
static volatile int frame_count = 0;

/* ---- focus assist (Tenengrad on luma) ---- */
static volatile float g_focus = 0.0f;
static volatile float g_focus_peak = 0.0f;

/* ---- media (implemented further down) ---- */
typedef struct avi_s avi_t;
static avi_t *g_avi;
static volatile int recording;
static int rec_frames;
static void avi_frame_unlocked(avi_t *a, const uint8_t *jpg, int len);
static pthread_mutex_t avi_lock = PTHREAD_MUTEX_INITIALIZER;
static void record_start(void);
static void record_stop(void);
static void photo_capture(void);
static char g_status_text[192] = "WiFi: --";
static void wifi_scan_async(void);
static int g_file_count;
static void files_scan(void);
static int media_remove(const char *name);
static void preview_open(const char *name);

#define FILE_MAX 60

struct mfile {
    char name[128];
    long size;
    time_t mtime;
};

static struct mfile g_files[FILE_MAX];

static volatile int page_req = -1;
static volatile int modal_req = 0;
static volatile int slide_req = 0;
static char *preview_req = NULL;
static volatile int files_dirty = 0;
/* ---------------- MJPEG AVI reader (for in-app playback) ---------------- */

struct avi_reader {
    FILE *f;
    long movi_pos, movi_end, pos;
};

static int avi_open_reader(const char *path, struct avi_reader *r)
{
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f) return -1;

    fseek(r->f, 0, SEEK_END);
    long fsize = ftell(r->f);

    long pos = 12;                      /* skip RIFF<size>AVI  */
    while (pos + 12 <= fsize) {
        fseek(r->f, pos, SEEK_SET);
        char id[5] = { 0 }, type[5] = { 0 };
        unsigned sz = 0;
        if (fread(id, 1, 4, r->f) != 4) break;
        if (fread(&sz, 4, 1, r->f) != 1) break;
        if (!memcmp(id, "LIST", 4)) {
            if (fread(type, 1, 4, r->f) != 4) break;
            if (!memcmp(type, "movi", 4)) {
                r->movi_pos = pos + 12;
                r->movi_end = pos + 8 + (long)sz;
                r->pos = r->movi_pos;
                return 0;
            }
        }
        pos += 8 + (long)sz + (sz & 1);
    }
    fclose(r->f);
    r->f = NULL;
    return -1;
}

static void avi_close_reader(struct avi_reader *r)
{
    if (r->f) fclose(r->f);
    r->f = NULL;
}

static void avi_rewind(struct avi_reader *r)
{
    r->pos = r->movi_pos;
}

/* returns a malloc'd JPEG frame, or -1 at end of stream */
static int avi_next_jpeg(struct avi_reader *r, uint8_t **out, size_t *out_len)
{
    if (!r->f) return -1;
    while (r->pos + 8 <= r->movi_end) {
        fseek(r->f, r->pos, SEEK_SET);
        char id[5] = { 0 };
        unsigned sz = 0;
        if (fread(id, 1, 4, r->f) != 4) return -1;
        if (fread(&sz, 4, 1, r->f) != 1) return -1;
        r->pos += 8;
        if (!memcmp(id, "00dc", 4)) {
            uint8_t *buf = malloc(sz ? sz : 1);
            if (!buf) return -1;
            if (fread(buf, 1, sz, r->f) != sz) { free(buf); return -1; }
            r->pos += (long)sz + (sz & 1);
            *out = buf;
            *out_len = sz;
            return 0;
        }
        r->pos += (long)sz + (sz & 1);
    }
    return -1;
}

static int jpeg_mem_to_rgb565(const uint8_t *jpg, size_t len, uint8_t *out, int *ow, int *oh)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width, h = cinfo.output_height;
    uint8_t *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&cinfo, rp, 1);
        uint8_t *dst = out + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(((row[x * 3] & 0xF8) << 8) |
                                    ((row[x * 3 + 1] & 0xFC) << 3) |
                                    (row[x * 3 + 2] >> 3));
            dst[x * 2] = v & 0xFF;
            dst[x * 2 + 1] = v >> 8;
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *ow = w;
    *oh = h;
    return 0;
}

/* --- playback state --- */
#define VID_W 320
#define VID_H 240

static struct avi_reader vid_reader;
static volatile int vid_playing = 0;
static volatile int vid_stop = 0;
static volatile int vid_ready = 0;
static uint8_t vid_tmp[VID_W * VID_H * 2];
static uint8_t vid_show[VID_W * VID_H * 2];
static pthread_t vid_tid;
static int vid_thread_live = 0;
static lv_obj_t *preview_play;

static void *video_preview_thread(void *arg)
{
    (void)arg;
    uint8_t *jpg = NULL;
    size_t n = 0;

    while (!vid_stop) {
        if (!vid_playing) {
            usleep(50 * 1000);
            continue;
        }
        if (avi_next_jpeg(&vid_reader, &jpg, &n) != 0) {
            avi_rewind(&vid_reader);
            if (avi_next_jpeg(&vid_reader, &jpg, &n) != 0) {
                vid_playing = 0;
                continue;
            }
        }
        int w = 0, h = 0;
        if (jpeg_mem_to_rgb565(jpg, n, vid_tmp, &w, &h) == 0 &&
            w == VID_W && h == VID_H) {
            pthread_mutex_lock(&lock);
            memcpy(vid_show, vid_tmp, sizeof(vid_show));
            vid_ready = 1;
            pthread_mutex_unlock(&lock);
        }
        free(jpg);
        jpg = NULL;
        usleep(40 * 1000);          /* ~25 fps */
    }
    return NULL;
}

static void video_preview_stop(void)
{
    vid_playing = 0;
    vid_stop = 1;
    if (vid_thread_live) {
        pthread_join(vid_tid, NULL);
        vid_thread_live = 0;
    }
    avi_close_reader(&vid_reader);
    vid_ready = 0;
}

static void files_rebuild(void);

struct temps {
    double max, min, avg;
    double max_x, max_y;      /* normalised 0..1 */
    double min_x, min_y;
    int valid;
    int version;
};
static struct temps g_temps;

struct center_temp {
    float value;
    int valid;
    int version;
};
static struct center_temp g_center;

LV_FONT_DECLARE(lv_font_cn_22);
#define FONT_CN (font_cn ? font_cn : &lv_font_cn_22)

/* one family for both CJK and Latin so text on the same line always aligns;
 * falls back to the compiled bitmap subset when no TTF is available */
static lv_font_t *font_cn;
static lv_font_t *font_sm;
#define FONT_SM (font_sm ? font_sm : (font_cn ? font_cn : &lv_font_cn_22))

static void font_init(void)
{
    /* LVGL may already have initialised freetype itself, so ignore the result */
    lv_freetype_init(0);
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/home/linaro/linux-app/fonts/simhei.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        font_cn = lv_freetype_font_create(candidates[i], LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                          26, LV_FREETYPE_FONT_STYLE_NORMAL);
        font_sm = lv_freetype_font_create(candidates[i], LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                          20, LV_FREETYPE_FONT_STYLE_NORMAL);
        if (font_cn) {
            printf("font: loaded %s size 26/20\n", candidates[i]);
            fflush(stdout);
            return;
        }
    }
    printf("font: no TTF found, using bitmap subset\n");
}

static lv_obj_t *img_obj;
static lv_obj_t *lbl_stat;
static lv_obj_t *btn_pal, *lbl_pal;
static lv_obj_t *btn_clear;
static lv_obj_t *btn_photo, *lbl_rec;
static lv_obj_t *focus_track, *focus_fill, *focus_peak;
static lv_obj_t *sl_bright, *sl_contrast, *sl_screen;
static lv_obj_t *img_preview_sm;
#define SMALL_W 320
#define SMALL_H 240
static uint8_t img_small[SMALL_W * SMALL_H * 2];
static lv_image_dsc_t img_small_dsc;
static volatile int bright_pending = -1, contrast_pending = -1;
static int bright_val = 45, contrast_val = 45;
static volatile int color_synced = 0;
static lv_obj_t *lbl_hint;
static lv_obj_t *dot_max, *dot_min, *dot_ctr;
static lv_obj_t *tag_max, *tag_min, *tag_ctr;
static lv_obj_t *sw_btn[3], *sw_lbl[3];

#define MAX_SPOTS 8
struct spot {
    int x, y;              /* position inside the image (view coordinates) */
    float t;
    int valid;
    lv_obj_t *dot;
    lv_obj_t *lbl;
};
static struct spot spots[MAX_SPOTS];
static int spot_count = 0;
static int drag_idx = -1;
static int drag_moved = 0;
static lv_obj_t *img_touch;   /* transparent hit area over the image */
static int sw_on[3] = { 1, 1, 1 };      /* 最高 / 最低 / 中心 */
static const char *PAL_CN[] = {"白热", "黑热", "铁红1", "铁红2", "彩虹", "融合1", "融合2", "棕褐"};
static int shown_marker_ver = -1, shown_center_ver = -1;

static lv_image_dsc_t img_dsc;
static int shown_version = -1;

/* ---------------- HTTP ---------------- */

struct membuf {
    uint8_t *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct membuf *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->len + n) cap *= 2;
        uint8_t *p = realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    return n;
}

static int http_get(CURL *curl, const char *url, struct membuf *buf, long timeout_s)
{
    buf->len = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD, AUTH);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return (rc == CURLE_OK && code == 200) ? 0 : -1;
}

/* ---------------- JPEG -> RGB565 ---------------- */

struct jpeg_err {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jpeg_error_exit_cb(j_common_ptr cinfo)
{
    struct jpeg_err *e = (struct jpeg_err *)cinfo->err;
    longjmp(e->jb, 1);
}

static __attribute__((unused)) int decode_jpeg_rgb565(const uint8_t *jpg, size_t len, uint8_t *out)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_err jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_cb;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width != IMG_W || cinfo.output_height != IMG_H) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    int row = cinfo.output_width * cinfo.output_components;
    uint8_t *line = malloc(row);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *rows[1] = { line };
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint8_t *dst = out + (size_t)(cinfo.output_scanline - 1) * IMG_W * 2;
        for (int x = 0; x < IMG_W; x++) {
            uint8_t r = line[x * 3 + 0], g = line[x * 3 + 1], b = line[x * 3 + 2];
            uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            dst[x * 2 + 0] = (uint8_t)(v & 0xFF);
            dst[x * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
    free(line);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* ---------------- worker threads ---------------- */

/* ---------------- RTSP (libavformat) ---------------- */ /* tweak */

static AVFormatContext *g_fmt;
static AVCodecContext *g_dec;
static struct SwsContext *g_sws;
static int g_vidx = -1;

/* newest raw MJPEG packet + viewer count, shared with the web stream */
static volatile int stream_viewers = 0;
static pthread_mutex_t raw_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *raw_jpg;
static size_t raw_len;
static int raw_ver;



static int rtsp_open_stream(void)
{
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "3000000", 0);      /* microseconds */
    av_dict_set(&opts, "max_delay", "500000", 0);

    if (avformat_open_input(&g_fmt, RTSP_URL, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return -1;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(g_fmt, NULL) < 0) return -1;
    g_vidx = av_find_best_stream(g_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (g_vidx < 0) return -1;

    AVCodecParameters *par = g_fmt->streams[g_vidx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return -1;

    g_dec = avcodec_alloc_context3(codec);
    if (!g_dec) return -1;
    avcodec_parameters_to_context(g_dec, par);
    g_dec->thread_count = 2;
    if (avcodec_open2(g_dec, codec, NULL) < 0) return -1;

    g_sws = sws_getContext(g_dec->width, g_dec->height, g_dec->pix_fmt,
                           VIEW_W, VIEW_H, AV_PIX_FMT_RGB565LE,
                           SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_sws) return -1;

    printf("rtsp: %s %dx%d codec=%s\n", g_fmt->iformat->name,
           g_dec->width, g_dec->height, codec->name);
    fflush(stdout);
    return 0;
}

static void rtsp_close_stream(void)
{
    if (g_sws) { sws_freeContext(g_sws); g_sws = NULL; }
    if (g_dec) { avcodec_free_context(&g_dec); }
    if (g_fmt) { avformat_close_input(&g_fmt); }
    g_vidx = -1;
}


/* average squared Sobel gradient of the luma channel, subsampled 4x */
static void compute_focus(const uint8_t *rgb565)
{
    const int step = 4;
    long acc = 0;
    int n = 0;

    for (int y = step; y < VIEW_H - step; y += step) {
        const uint8_t *r0 = rgb565 + ((size_t)(y - step) * VIEW_W) * 2;
        const uint8_t *r1 = rgb565 + ((size_t)y * VIEW_W) * 2;
        const uint8_t *r2 = rgb565 + ((size_t)(y + step) * VIEW_W) * 2;
        for (int x = step; x < VIEW_W - step; x += step) {
            int i0 = x * 2, i1 = (x - step) * 2, i2 = (x + step) * 2;
            int l00 = ((r0[i1] | (r0[i1 + 1] << 8)) >> 5) & 0x3F;
            int l01 = ((r0[i0] | (r0[i0 + 1] << 8)) >> 5) & 0x3F;
            int l02 = ((r0[i2] | (r0[i2 + 1] << 8)) >> 5) & 0x3F;
            int l10 = ((r1[i1] | (r1[i1 + 1] << 8)) >> 5) & 0x3F;
            int l12 = ((r1[i2] | (r1[i2 + 1] << 8)) >> 5) & 0x3F;
            int l20 = ((r2[i1] | (r2[i1 + 1] << 8)) >> 5) & 0x3F;
            int l21 = ((r2[i0] | (r2[i0 + 1] << 8)) >> 5) & 0x3F;
            int l22 = ((r2[i2] | (r2[i2 + 1] << 8)) >> 5) & 0x3F;

            int gx = (l02 + 2 * l12 + l22) - (l00 + 2 * l10 + l20);
            int gy = (l20 + 2 * l21 + l22) - (l00 + 2 * l01 + l02);
            acc += gx * gx + gy * gy;
            n++;
        }
    }

    float m = n ? (float)acc / n : 0.0f;
    if (m > g_focus_peak) g_focus_peak = m;
    else g_focus_peak = g_focus_peak * 0.999f;   /* slow decay so the peak follows the scene */
    if (g_focus_peak < 1.0f) g_focus_peak = 1.0f;
    g_focus = m;
}

static void *video_thread(void *arg)
{
    (void)arg;
    avformat_network_init();

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();

    while (running) {
        if (!g_fmt && rtsp_open_stream() != 0) {
            usleep(1000 * 1000);
            continue;
        }

        if (av_read_frame(g_fmt, pkt) < 0) {
            printf("rtsp: read failed, reconnecting\n");
            fflush(stdout);
            rtsp_close_stream();
            usleep(500 * 1000);
            continue;
        }

        if (pkt->stream_index != g_vidx) {
            av_packet_unref(pkt);
            continue;
        }

        if (stream_viewers > 0 && pkt->size > 0 && pkt->size < 512 * 1024) {
            pthread_mutex_lock(&raw_lock);
            uint8_t *nb = realloc(raw_jpg, (size_t)pkt->size);
            if (nb) {
                raw_jpg = nb;
                memcpy(raw_jpg, pkt->data, (size_t)pkt->size);
                raw_len = (size_t)pkt->size;
                raw_ver++;
            }
            pthread_mutex_unlock(&raw_lock);
        }

        if (recording) {
            pthread_mutex_lock(&avi_lock);
            if (g_avi) { avi_frame_unlocked(g_avi, pkt->data, pkt->size); rec_frames++; }
            pthread_mutex_unlock(&avi_lock);
        }

        if (avcodec_send_packet(g_dec, pkt) == 0) {
            while (avcodec_receive_frame(g_dec, frm) == 0) {
                uint8_t *dst[4] = { frame_tmp, NULL, NULL, NULL };
                int dst_stride[4] = { VIEW_W * 2, 0, 0, 0 };
                sws_scale(g_sws, (const uint8_t * const *)frm->data, frm->linesize,
                          0, frm->height, dst, dst_stride);
                compute_focus(frame_tmp);
                pthread_mutex_lock(&lock);
                memcpy(frame_pending, frame_tmp, sizeof(frame_pending));
                frame_ready = 1;
                pthread_mutex_unlock(&lock);
                frame_count++;
                av_frame_unref(frm);
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frm);
    av_packet_free(&pkt);
    rtsp_close_stream();
    return NULL;
}

static int parse_double(const char *json, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    *out = atof(p + strlen(pat));
    return 0;
}

/* parse positionX/positionY of the first point object after `from` */
static int parse_point_at(const char *from, double *x, double *y)
{
    if (!from) return -1;
    const char *p = strstr(from, "positionX");
    if (!p) return -1;
    *x = atof(strchr(p, ':') + 1);
    p = strstr(p, "positionY");
    if (!p) return -1;
    *y = atof(strchr(p, ':') + 1);
    return 0;
}

#define MAT_W 160
#define MAT_H 120
#define MAT_BYTES (MAT_W * MAT_H * 4)

/* pull jpegPicWithAppendData and read the centre pixel of the float matrix */
static void *matrix_thread(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    struct membuf buf = { 0 };
    while (running) {
        if (http_get(curl, MODULE_URL "/ISAPI/Thermal/channels/1/thermometry/jpegPicWithAppendData?format=json",
                     &buf, 8) == 0) {
            /* find the octet-stream part, then its body */
            const char *tag = "Content-Type: application/octet-stream";
            uint8_t *hit = memmem(buf.data, buf.len, tag, strlen(tag));
            if (hit) {
                uint8_t *body = memmem(hit, buf.data + buf.len - hit, "\r\n\r\n", 4);
                if (body && (size_t)(buf.data + buf.len - (body + 4)) >= MAT_BYTES) {
                    const float *m = (const float *)(body + 4);
                    float c = m[(MAT_H / 2) * MAT_W + (MAT_W / 2)];
                    pthread_mutex_lock(&lock);
                    g_center.value = c;
                    g_center.valid = 1;
                    g_center.version++;
                    for (int i = 0; i < spot_count; i++) {
                        int sx = spots[i].x * MAT_W / VIEW_W;
                        int sy = spots[i].y * MAT_H / VIEW_H;
                        if (sx < 0) sx = 0;
                        if (sx >= MAT_W) sx = MAT_W - 1;
                        if (sy < 0) sy = 0;
                        if (sy >= MAT_H) sy = MAT_H - 1;
                        spots[i].t = m[sy * MAT_W + sx];
                        spots[i].valid = 1;
                    }
                    pthread_mutex_unlock(&lock);
                }
            }
        }
        usleep(900 * 1000);
    }
    free(buf.data);
    curl_easy_cleanup(curl);
    return NULL;
}

static void *temp_thread(void *arg)
{
    (void)arg;
    CURL *curl = curl_easy_init();
    struct membuf buf = { 0 };
    while (running) {
        if (http_get(curl, TEMP_URL, &buf, 5) == 0) {
            char *json = malloc(buf.len + 1);
            memcpy(json, buf.data, buf.len);
            json[buf.len] = 0;
            double mx, mn, av, px = 0.5, py = 0.5, nx = 0.5, ny = 0.5;
            if (parse_double(json, "maxTemperature", &mx) == 0 &&
                parse_double(json, "minTemperature", &mn) == 0 &&
                parse_double(json, "averageTemperature", &av) == 0) {
                parse_point_at(strstr(json, "\"MaxTemperaturePoint\""), &px, &py);
                parse_point_at(strstr(json, "\"MinTemperaturePoint\""), &nx, &ny);
                pthread_mutex_lock(&lock);
                g_temps.max = mx;
                g_temps.min = mn;
                g_temps.avg = av;
                g_temps.max_x = px; g_temps.max_y = py;
                g_temps.min_x = nx; g_temps.min_y = ny;
                g_temps.valid = 1;
                g_temps.version++;
                pthread_mutex_unlock(&lock);
            }
            free(json);
        }
        usleep(200 * 1000);
    }
    free(buf.data);
    curl_easy_cleanup(curl);
    return NULL;
}

/* ---------------- palette switching (ISAPI PUT) ---------------- */

static const char *PALETTES[] = {
    "WhiteHot", "BlackHot", "Ironbow1", "Ironbow2",
    "Rainbow", "Fusion1", "Fusion2", "Sepia"
};
#define PAL_COUNT ((int)(sizeof(PALETTES) / sizeof(PALETTES[0])))

static volatile int pal_current = 2;   /* Ironbow1, set in the module config */
static volatile int pal_applied = -1;

static void *palette_thread(void *arg)
{
    int idx = (int)(intptr_t)arg;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char body[256];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             "<Palettes version=\"2.0\" xmlns=\"http://www.std-cgi.com/ver20/XMLSchema\">"
             "<mode>%s</mode></Palettes>", PALETTES[idx]);

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Content-Type: application/xml; charset=UTF-8");

    curl_easy_setopt(curl, CURLOPT_URL, MODULE_URL "/ISAPI/Image/channels/1/Palettes");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD, AUTH);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    printf("palette -> %s : %s (http %ld)\n", PALETTES[idx],
           rc == CURLE_OK ? "ok" : curl_easy_strerror(rc), code);
    fflush(stdout);
    if (rc == CURLE_OK && code == 200) pal_applied = idx;

    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return NULL;
}

static void on_pal_click(lv_event_t *e)
{
    (void)e;
    int next = (pal_current + 1) % PAL_COUNT;
    pal_current = next;
    lv_label_set_text(lbl_pal, PAL_CN[next]);

    pthread_t th;
    if (pthread_create(&th, NULL, palette_thread, (void *)(intptr_t)next) == 0) {
        pthread_detach(th);
    }
}

/* ---------------- brightness / contrast via /ISAPI/Image/channels/1/Color ---------------- */

static int color_put(int brightness, int contrast)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char body[256];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             "<Color version=\"2.0\" xmlns=\"http://www.std-cgi.com/ver20/XMLSchema\">"
             "<brightnessLevel>%d</brightnessLevel>"
             "<contrastLevel>%d</contrastLevel></Color>", brightness, contrast);

    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Content-Type: application/xml; charset=UTF-8");

    curl_easy_setopt(curl, CURLOPT_URL, MODULE_URL "/ISAPI/Image/channels/1/Color");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD, AUTH);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return (rc == CURLE_OK && http == 200) ? 0 : -1;
}

static void color_load(void)
{
    CURL *curl = curl_easy_init();
    if (!curl) return;
    struct membuf buf = { 0 };
    if (http_get(curl, MODULE_URL "/ISAPI/Image/channels/1/Color", &buf, 5) == 0) {
        char *xml = malloc(buf.len + 1);
        memcpy(xml, buf.data, buf.len);
        xml[buf.len] = 0;
        const char *p = strstr(xml, "brightnessLevel");
        if (p) bright_val = atoi(strchr(p, '>') + 1);
        p = strstr(xml, "contrastLevel");
        if (p) contrast_val = atoi(strchr(p, '>') + 1);
        printf("color loaded: brightness=%d contrast=%d\n", bright_val, contrast_val);
        fflush(stdout);
        color_synced = 1;
        free(xml);
    }
    free(buf.data);
    curl_easy_cleanup(curl);
}

static void *color_thread(void *arg)
{
    (void)arg;
    color_load();
    while (running) {
        int b = bright_pending, c = contrast_pending;
        if (b >= 0 || c >= 0) {
            int nb = (b >= 0) ? b : bright_val;
            int nc = (c >= 0) ? c : contrast_val;
            bright_pending = -1;
            contrast_pending = -1;
            if (color_put(nb, nc) == 0) {
                bright_val = nb;
                contrast_val = nc;
                printf("color -> brightness=%d contrast=%d\n", nb, nc);
            } else {
                printf("color PUT failed (%d,%d)\n", nb, nc);
            }
            fflush(stdout);
        }
        usleep(120 * 1000);
    }
    return NULL;
}

/* ---------------- panel backlight: GP7101 (I2C1 @0x58, 8-bit PWM in reg 0x03) ----------------
 * The 3.1" panel backlight is NOT on the SoC PWM: the expansion board feeds it through a
 * SY7201 boost driver whose DIM pin is driven by GP7101, an I2C-to-PWM chip on i2c-1.
 * Write [0x03, 0..255] to 0x58 to set the duty cycle.
 */

#define GP7101_BUS     "/dev/i2c-1"
#define GP7101_ADDR    0x58
#define GP7101_REG_8   0x03
#define BL_SAVE        "/home/linaro/linux-app/.screen_bright"

static volatile int screen_pending = -1;
static int screen_pct = 70;
static int bl_i2c_fd = -1;

static int gp7101_open(void)
{
    if (bl_i2c_fd >= 0) return 0;
    int fd = open(GP7101_BUS, O_RDWR);
    if (fd < 0) {
        printf("gp7101: cannot open %s: %s\n", GP7101_BUS, strerror(errno));
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, GP7101_ADDR) < 0) {
        printf("gp7101: ioctl I2C_SLAVE failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    bl_i2c_fd = fd;
    return 0;
}

static void screen_init(void)
{
    FILE *f = fopen(BL_SAVE, "r");
    if (f) {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 100) screen_pct = v;
        fclose(f);
    }
    printf("backlight: gp7101 i2c-1@0x58, saved=%d%%\n", screen_pct);
    fflush(stdout);
}

static void screen_set(int pct)
{
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;

    /* the board's LED driver is wired to GP7101's inverted output:
     * a larger PWM value means DARKER, so the duty must be mirrored */
    int raw = (100 - pct) * 255 / 100;
    if (raw < 0) raw = 0;
    if (raw > 254) raw = 254;

    if (gp7101_open() == 0) {
        uint8_t buf[2] = { GP7101_REG_8, (uint8_t)raw };
        if (write(bl_i2c_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
            printf("gp7101: write failed: %s\n", strerror(errno));
    }

    FILE *f = fopen(BL_SAVE, "w");
    if (f) {
        fprintf(f, "%d\n", pct);
        fclose(f);
    }
    screen_pct = pct;
    printf("backlight -> %d%% (raw %d)\n", pct, raw);
    fflush(stdout);
}

/* ---------------- media: photo, AVI recording, web server ---------------- */

#define MEDIA_DIR "/home/linaro/linux-app/media"

static void media_init(void)
{
    mkdir(MEDIA_DIR, 0755);
}

static void stamp_now(char *buf, size_t n)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, n, "%Y%m%d_%H%M%S", &tm);
}

/* ---- JPEG encoder (RGB565 buffer -> file) ---- */
static int save_jpeg_rgb565(const char *path, const uint8_t *rgb565, int w, int h, int quality)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    uint8_t *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgb565 + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            row[x * 3 + 0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            row[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = (uint8_t)((v & 0x1F) << 3);
        }
        JSAMPROW rp[1] = { row };
        jpeg_write_scanlines(&cinfo, rp, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return 0;
}

static void photo_capture(void)
{
    static uint8_t snap[VIEW_W * VIEW_H * 2];
    pthread_mutex_lock(&lock);
    memcpy(snap, frame_show, sizeof(snap));
    double mx = g_temps.max, mn = g_temps.min, av = g_temps.avg;
    float ctr = g_center.value;
    int ctr_ok = g_center.valid;
    pthread_mutex_unlock(&lock);

    char ts[32], path[256], json[256];
    stamp_now(ts, sizeof(ts));
    snprintf(path, sizeof(path), MEDIA_DIR "/photo_%s.jpg", ts);
    if (save_jpeg_rgb565(path, snap, VIEW_W, VIEW_H, 90) == 0) {
        snprintf(json, sizeof(json), MEDIA_DIR "/photo_%s.json", ts);
        FILE *f = fopen(json, "w");
        if (f) {
            fprintf(f, "{\"max\":%.1f,\"min\":%.1f,\"avg\":%.1f,\"center\":%.1f}\n",
                    mx, mn, av, ctr_ok ? ctr : -999.0f);
            fclose(f);
        }
        printf("photo saved: %s\n", path);
        files_dirty = 1;
    } else {
        printf("photo failed: %s\n", path);
    }
    fflush(stdout);
}

/* ---- minimal AVI (MJPEG) writer ---- */
struct avi_s {
    FILE *f;
    int w, h, fps, frames;
    long riff_pos, tot_pos, movi_size_pos, movi_start;
    long *offs, *sizes;
    int cap;
};

static void avi_w4(avi_t *a, unsigned v)
{
    fputc(v & 0xFF, a->f);
    fputc((v >> 8) & 0xFF, a->f);
    fputc((v >> 16) & 0xFF, a->f);
    fputc((v >> 24) & 0xFF, a->f);
}

static void avi_tag(avi_t *a, const char *s)
{
    for (int i = 0; i < 4; i++) fputc(s[i], a->f);
}

static avi_t *avi_open(const char *path, int w, int h, int fps)
{
    avi_t *a = calloc(1, sizeof(*a));
    a->f = fopen(path, "wb");
    if (!a->f) { free(a); return NULL; }
    a->w = w; a->h = h; a->fps = fps;

    avi_tag(a, "RIFF");
    a->riff_pos = ftell(a->f);
    avi_w4(a, 0);
    avi_tag(a, "AVI ");

    avi_tag(a, "LIST"); avi_w4(a, 4 + 8 + 56 + 4 + 8 + 56 + 40); avi_tag(a, "hdrl");
    avi_tag(a, "avih"); avi_w4(a, 56);
    avi_w4(a, 1000000 / fps); avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0x10);
    a->tot_pos = ftell(a->f); avi_w4(a, 0);
    avi_w4(a, 0); avi_w4(a, 1); avi_w4(a, w * h * 3); avi_w4(a, w); avi_w4(a, h);
    avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0);

    avi_tag(a, "LIST"); avi_w4(a, 4 + 8 + 56 + 8 + 40); avi_tag(a, "strl");
    avi_tag(a, "strh"); avi_w4(a, 56);
    avi_tag(a, "vids"); avi_tag(a, "MJPG");
    avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0);
    avi_w4(a, 1); avi_w4(a, fps); avi_w4(a, 0); avi_w4(a, 0);
    avi_w4(a, w * h * 3); avi_w4(a, 0); avi_w4(a, 0);
    avi_w4(a, 0); avi_w4(a, (h << 16) | w);

    avi_tag(a, "strf"); avi_w4(a, 40);
    avi_w4(a, 40); avi_w4(a, w); avi_w4(a, h); avi_w4(a, 0x00180001);
    avi_tag(a, "MJPG");
    avi_w4(a, w * h * 3); avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0); avi_w4(a, 0);

    avi_tag(a, "LIST");
    a->movi_size_pos = ftell(a->f);
    avi_w4(a, 0);
    avi_tag(a, "movi");
    a->movi_start = ftell(a->f);
    printf("avi open: %s %dx%d\n", path, w, h);
    fflush(stdout);
    return a;
}

static void avi_frame_unlocked(avi_t *a, const uint8_t *jpg, int len)
{
    if (!a) return;
    if (a->frames >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->offs = realloc(a->offs, a->cap * sizeof(long));
        a->sizes = realloc(a->sizes, a->cap * sizeof(long));
    }
    a->offs[a->frames] = ftell(a->f) - a->movi_start;
    a->sizes[a->frames] = len;
    avi_tag(a, "00dc");
    avi_w4(a, len);
    fwrite(jpg, 1, len, a->f);
    if (len & 1) fputc(0, a->f);
    a->frames++;
}

static void avi_close(avi_t *a)
{
    if (!a) return;
    long idx = ftell(a->f);
    avi_tag(a, "idx1"); avi_w4(a, a->frames * 16);
    for (int i = 0; i < a->frames; i++) {
        avi_tag(a, "00dc"); avi_w4(a, 0x10);
        avi_w4(a, (unsigned)a->offs[i]); avi_w4(a, (unsigned)a->sizes[i]);
    }
    long end = ftell(a->f);

    fseek(a->f, a->movi_size_pos, SEEK_SET);
    avi_w4(a, (unsigned)(idx - a->movi_size_pos - 4));
    fseek(a->f, a->tot_pos, SEEK_SET);
    avi_w4(a, a->frames);
    fseek(a->f, a->riff_pos, SEEK_SET);
    avi_w4(a, (unsigned)(end - 8));
    fclose(a->f);
    printf("avi closed: %d frames\n", a->frames);
    fflush(stdout);
    free(a->offs);
    free(a->sizes);
    free(a);
}

static void record_start(void)
{
    if (recording) return;
    char ts[32], path[256];
    stamp_now(ts, sizeof(ts));
    snprintf(path, sizeof(path), MEDIA_DIR "/video_%s.avi", ts);
    int w = (g_dec && g_dec->width) ? g_dec->width : IMG_W;
    int h = (g_dec && g_dec->height) ? g_dec->height : IMG_H;
    pthread_mutex_lock(&avi_lock);
    g_avi = avi_open(path, w, h, 25);
    if (!g_avi) { pthread_mutex_unlock(&avi_lock); printf("avi open failed\n"); return; }
    rec_frames = 0;
    recording = 1;
    pthread_mutex_unlock(&avi_lock);
}

static void record_stop(void)
{
    if (!recording) return;
    pthread_mutex_lock(&avi_lock);
    recording = 0;
    avi_close(g_avi);
    g_avi = NULL;
    pthread_mutex_unlock(&avi_lock);
    files_dirty = 1;
}

/* ---- tiny HTTP server: GET / (listing) and /files/<name> ---- */
static void http_send(int fd, const char *status, const char *ctype, const uint8_t *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", status, ctype, len);
    send(fd, head, n, 0);
    if (body && len) send(fd, body, len, 0);
}

/* ---- first MJPEG frame of an AVI, served as a JPEG poster ---- */
static void http_poster(int fd, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), MEDIA_DIR "/%s", name);
    struct avi_reader r;
    if (avi_open_reader(path, &r) != 0) {
        http_send(fd, "404 Not Found", "text/plain", (const uint8_t *)"no", 2);
        return;
    }
    uint8_t *jpg = NULL;
    size_t n = 0;
    if (avi_next_jpeg(&r, &jpg, &n) == 0) {
        http_send(fd, "200 OK", "image/jpeg", jpg, n);
        free(jpg);
    } else {
        http_send(fd, "404 Not Found", "text/plain", (const uint8_t *)"no", 2);
    }
    avi_close_reader(&r);
}

/* ---- CRC32 for the zip writer ---- */
static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_file(const char *path, long *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t buf[32768];
    size_t got;
    long total = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < got; i++) crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        total += (long)got;
    }
    fclose(f);
    if (size_out) *size_out = total;
    return crc ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}

#define ZIP_MAX 32

static void http_zip(int fd, char names[][128], int n)
{
    if (!crc_ready) crc_init();
    if (n <= 0 || n > ZIP_MAX) {
        http_send(fd, "400 Bad Request", "text/plain", (const uint8_t *)"empty", 5);
        return;
    }

    long sizes[ZIP_MAX];
    uint32_t crcs[ZIP_MAX];
    long total = 0;
    for (int i = 0; i < n; i++) {
        char path[512];
        snprintf(path, sizeof(path), MEDIA_DIR "/%s", names[i]);
        sizes[i] = 0;
        crcs[i] = crc32_file(path, &sizes[i]);
        if (sizes[i] <= 0) { crcs[i] = 0; }
        total += 30 + (long)strlen(names[i]) + sizes[i];
    }
    for (int i = 0; i < n; i++) total += 46 + (long)strlen(names[i]);
    total += 22;

    char head[256];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                      "Content-Disposition: attachment; filename=\"4117-photos.zip\"\r\n"
                      "Content-Length: %ld\r\nConnection: close\r\n\r\n", total);
    send(fd, head, hn, 0);

    long *offsets = calloc((size_t)n, sizeof(long));
    long pos = 0;

    for (int i = 0; i < n; i++) {
        uint8_t h[30];
        uint16_t nlen = (uint16_t)strlen(names[i]);
        put32(h + 0, 0x04034b50);
        put16(h + 4, 20);
        put16(h + 6, 0);
        put16(h + 8, 0);            /* store */
        put16(h + 10, 0);           /* time */
        put16(h + 12, 0x21);        /* 1980-01-01 */
        put32(h + 14, crcs[i]);
        put32(h + 18, (uint32_t)sizes[i]);
        put32(h + 22, (uint32_t)sizes[i]);
        put16(h + 26, nlen);
        put16(h + 28, 0);
        send(fd, h, 30, 0);
        send(fd, names[i], nlen, 0);

        offsets[i] = pos;
        pos += 30 + nlen;

        char path[512];
        snprintf(path, sizeof(path), MEDIA_DIR "/%s", names[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t buf[32768];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) send(fd, buf, got, 0);
            fclose(f);
        }
        pos += sizes[i];
    }

    for (int i = 0; i < n; i++) {
        uint8_t c[46];
        uint16_t nlen = (uint16_t)strlen(names[i]);
        put32(c + 0, 0x02014b50);
        put16(c + 4, 20);
        put16(c + 6, 20);
        put16(c + 8, 0);
        put16(c + 10, 0);
        put16(c + 12, 0);
        put16(c + 14, 0x21);
        put32(c + 16, crcs[i]);
        put32(c + 20, (uint32_t)sizes[i]);
        put32(c + 24, (uint32_t)sizes[i]);
        put16(c + 28, nlen);
        put16(c + 30, 0);
        put16(c + 32, 0);
        put16(c + 34, 0);
        put16(c + 36, 0);
        put32(c + 38, 0);
        put32(c + 42, (uint32_t)offsets[i]);
        send(fd, c, 46, 0);
        send(fd, names[i], nlen, 0);
    }

    uint8_t e[22];
    put32(e + 0, 0x06054b50);
    put16(e + 4, 0);
    put16(e + 6, 0);
    put16(e + 8, (uint16_t)n);
    put16(e + 10, (uint16_t)n);
    put32(e + 12, (uint32_t)(total - 22));
    put32(e + 16, (uint32_t)pos);
    put16(e + 20, 0);
    send(fd, e, 22, 0);

    free(offsets);
    printf("zip: %d files, %ld bytes\n", n, total);
    fflush(stdout);
}

static void http_files_json(int fd)
{
    static char body[8192];
    int n = snprintf(body, sizeof(body), "[");
    for (int i = 0; i < g_file_count && n < (int)sizeof(body) - 256; i++) {
        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"name\":\"%s\",\"size\":%ld,\"t\":%ld}",
                      i ? "," : "", g_files[i].name, g_files[i].size,
                      (long)g_files[i].mtime);
    }
    n += snprintf(body + n, sizeof(body) - n, "]");
    http_send(fd, "200 OK", "application/json", (const uint8_t *)body, n);
}

static void http_listing(int fd)
{
    static char body[16384];
    int n = snprintf(body, sizeof(body),
        "<!doctype html><meta charset=utf-8><title>4117\u70ed\u50cf\u4eea</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1d21;color:#ccc;margin:0;padding:16px}"
        "a{color:#4fc1ff;text-decoration:none}"
        "button{background:#007acc;color:#fff;border:0;border-radius:8px;"
        "padding:9px 14px;margin:4px 6px 4px 0;font-size:14px;cursor:pointer}"
        "button.red{background:#a31515}button.grey{background:#3a3f46}"
        "img#p{display:block;width:480px;image-rendering:pixelated;border:1px solid #333;"
        "border-radius:6px;margin:10px 0}"
        "#st{font-size:14px;color:#9cdcfe;margin:4px 0}"
        "#toast{height:20px;color:#8fd18f;font-size:14px}"
        "ul{padding:0;margin:8px 0}li.row{display:flex;align-items:center;gap:10px;"
        "padding:6px;border-bottom:1px solid #2a2f35}"
        "li.row img.thumb{width:64px;height:48px;object-fit:cover;border-radius:4px;"
        "border:1px solid #333;background:#000;cursor:pointer}"
        ".name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
        ".sz{color:#888;font-size:13px;width:74px;text-align:right}"
        "button.del{background:#5a1f1f;padding:6px 10px;font-size:13px;margin:0}"
        "input[type=checkbox]{width:18px;height:18px}"
        "</style>"
        "<h2 style='margin:0 0 6px'>4117 \u70ed\u50cf\u4eea</h2>"
        "<div id=st>\u72b6\u6001\u52a0\u8f7d\u4e2d</div>"
        "<div style='margin:6px 0'>"
        "<a href='/ISAPI' style='display:none'></a>"
        "<button class=grey onclick=\"window.open('http://'+location.hostname+':8081/','_blank')\">"
        "\u673a\u82af\u914d\u7f6e\u9875</button>"
        "<span class=sz style='margin-left:8px'>"
        "\u673a\u82af\u81ea\u5e26\u7684\u9884\u89c8\u9700\u8981 ActiveX \u63d2\u4ef6\uff0c"
        "\u73b0\u4ee3\u6d4f\u89c8\u5668\u5df2\u4e0d\u652f\u6301\uff1b"
        "\u9884\u89c8\u8bf7\u7528\u4e0a\u65b9\u753b\u9762</span>"
        "</div>"
        "<img id=p src=\"/stream.mjpg\">"
        "<div>"
        "<button onclick=\"act('/capture','\u5df2\u62cd\u7167')\">\u62cd\u7167</button>"
        "<button onclick=\"act('/record/start','\u5f00\u59cb\u5f55\u50cf')\">\u5f00\u59cb\u5f55\u50cf</button>"
        "<button class=red onclick=\"act('/record/stop','\u5df2\u505c\u6b62')\">\u505c\u6b62\u5f55\u50cf</button>"
        "</div>"
        "<div id=toast></div>"
        "<h3>\u6587\u4ef6</h3>"
        "<div>"
        "<button class=grey onclick=\"selAll(true)\">\u5168\u9009</button>"
        "<button class=grey onclick=\"selAll(false)\">\u53d6\u6d88\u5168\u9009</button>"
        "<button onclick=\"zipSel()\">\u4e0b\u8f7d\u9009\u4e2d</button>"
        "<button class=red onclick=\"del()\">\u5220\u9664\u9009\u4e2d</button>"
        "</div>"
        "<ul id=list></ul>"
        "<script>"
        "let files=[],checked=new Set();"
        "function toast(m){let t=document.getElementById('toast');t.textContent=m;"
        "setTimeout(()=>{if(t.textContent===m)t.textContent=''},2500)}"
        "function act(u,m){fetch(u).then(r=>r.text()).then(x=>{toast(m);list()})"
        ".catch(e=>toast('\u5931\u8d25: '+e))}"
        "function selAll(v){checked.clear();if(v)files.forEach(f=>checked.add(f.name));list()}"
        "function toggle(n){checked.has(n)?checked.delete(n):checked.add(n)}"
        "function del(one){let names=one?[one]:[...checked];"
        "if(!names.length){toast('\u5148\u9009\u4e2d\u6587\u4ef6');return}"
        "if(!confirm('\u5220\u9664 '+names.length+' \u4e2a\u6587\u4ef6?'))return;"
        "Promise.all(names.map(n=>fetch('/delete?f='+encodeURIComponent(n))))"
        ".then(()=>{toast('\u5df2\u5220\u9664 '+names.length+' \u4e2a');checked.clear();list()})}"
        "function zipSel(){let names=[...checked];"
        "if(!names.length){toast('\u5148\u9009\u4e2d\u6587\u4ef6');return}"
        "location.href='/zip?'+names.map(n=>'f='+encodeURIComponent(n)).join('&');"
        "toast('\u6b63\u5728\u6253\u5305 '+names.length+' \u4e2a\u6587\u4ef6')}"
        "function closeOv(){let o=document.getElementById('ov');if(o)o.remove()}"
        "function preview(src){closeOv();"
        "let ov=document.createElement('div');ov.id='ov';"
        "ov.style.cssText='position:fixed;inset:0;background:rgba(0,0,0,.88);display:flex;"
        "align-items:center;justify-content:center;z-index:9;cursor:zoom-out';"
        "ov.onclick=closeOv;"
        "let im=document.createElement('img');im.src=src;"
        "im.style.cssText='max-width:90%;max-height:86%;border:2px solid #444;border-radius:8px';"
        "im.onclick=e=>e.stopPropagation();"
        "let b=document.createElement('button');b.textContent='关闭';"
        "b.style.cssText='position:fixed;top:14px;right:14px;z-index:10;background:#a31515';"
        "b.onclick=closeOv;"
        "ov.append(im,b);document.body.appendChild(ov);"
        "document.onkeydown=e=>{if(e.key==='Escape')closeOv()}}"
        "function list(){fetch('/files.json').then(r=>r.json()).then(fs=>{files=fs;"
        "let u=document.getElementById('list');u.innerHTML='';"
        "if(!fs.length){u.innerHTML='<li class=sz>\u6682\u65e0\u6587\u4ef6</li>';return}"
        "fs.forEach(f=>{let li=document.createElement('li');li.className='row';"
        "let cb=document.createElement('input');cb.type='checkbox';cb.checked=checked.has(f.name);"
        "cb.onchange=()=>toggle(f.name);"
        "let th=document.createElement('img');th.className='thumb';"
        "th.src=f.name.endsWith('.avi')?('/poster?f='+encodeURIComponent(f.name))"
        ":('/files/'+encodeURIComponent(f.name));th.onclick=()=>preview(th.src);"
        "let a=document.createElement('a');a.className='name';a.href='/files/'+f.name;"
        "a.textContent=f.name;"
        "let s=document.createElement('span');s.className='sz';"
        "s.textContent=(f.size/1024).toFixed(0)+' KB';"
        "let b=document.createElement('button');b.className='del';b.textContent='\u5220\u9664';"
        "b.onclick=()=>del(f.name);li.append(cb,th,a,s,b);u.append(li)})})}"
        "function st(){fetch('/status').then(r=>r.json()).then(s=>{"
        "document.getElementById('st').textContent="
        "'\u72b6\u6001: '+(s.recording?'\u5f55\u50cf\u4e2d':'idle')"
        "+'   \u5e27\u6570 '+s.frames+'   \u6587\u4ef6 '+s.files+'   \u5bf9\u7126 '+s.focus})}"
        "let pv=document.getElementById('p');"
        "pv.onerror=()=>{setTimeout(()=>{pv.src='/stream.mjpg?'+Date.now()},1500)};"
        "setInterval(st,1000);setInterval(()=>{if(!document.querySelector('img[style*=fixed]'))list()},5000);"
        "st();list();"
        "</script>");

    http_send(fd, "200 OK", "text/html; charset=utf-8", (const uint8_t *)body, n);
}

static int preview_jpeg(const uint8_t **out, size_t *out_len)
{
    static uint8_t *cache;
    static size_t cache_len;
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long age_ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (cache && age_ms >= 0 && age_ms < 200) {
        *out = cache;
        *out_len = cache_len;
        return 0;
    }

    static uint8_t snap[VIEW_W * VIEW_H * 2];
    pthread_mutex_lock(&lock);
    memcpy(snap, frame_show, sizeof(snap));
    pthread_mutex_unlock(&lock);
    if (frame_count < 2) return -1;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *mem = NULL;
    unsigned long mem_size = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &mem_size);

    cinfo.image_width = VIEW_W;
    cinfo.image_height = VIEW_H;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 75, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    uint8_t *row = malloc(VIEW_W * 3);
    for (int y = 0; y < VIEW_H; y++) {
        const uint8_t *src = snap + (size_t)y * VIEW_W * 2;
        for (int x = 0; x < VIEW_W; x++) {
            uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            row[x * 3 + 0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            row[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = (uint8_t)((v & 0x1F) << 3);
        }
        JSAMPROW rp[1] = { row };
        jpeg_write_scanlines(&cinfo, rp, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    free(cache);
    cache = malloc(mem_size);
    memcpy(cache, mem, mem_size);
    cache_len = mem_size;
    free(mem);
    last = now;

    *out = cache;
    *out_len = cache_len;
    return 0;
}

/* ---- MJPEG stream: one long-lived connection, frames pushed as they arrive ----
 * The RTSP source is already MJPEG, so we forward the camera's own JPEG frames
 * instead of re-encoding: zero extra CPU.
 */

static int encode_rgb565_jpeg(const uint8_t *rgb565, int w, int h, int quality,
                              unsigned char **out, unsigned long *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *mem = NULL;
    unsigned long mem_size = 0;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &mem_size);

    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    uint8_t *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgb565 + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            row[x * 3 + 0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            row[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
            row[x * 3 + 2] = (uint8_t)((v & 0x1F) << 3);
        }
        JSAMPROW rp[1] = { row };
        jpeg_write_scanlines(&cinfo, rp, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    *out = mem;
    *out_len = mem_size;
    return 0;
}

static void *mjpeg_stream_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static const char hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n";
    if (send(fd, hdr, sizeof(hdr) - 1, 0) < 0) {
        close(fd);
        return NULL;
    }

    static uint8_t sbuf[512 * 1024];
    int last_ver = -1;

    while (running) {
        size_t len = 0;
        pthread_mutex_lock(&raw_lock);
        if (raw_jpg && raw_ver != last_ver) {
            len = raw_len;
            if (len > sizeof(sbuf)) len = sizeof(sbuf);
            memcpy(sbuf, raw_jpg, len);
            last_ver = raw_ver;
        }
        pthread_mutex_unlock(&raw_lock);

        if (len == 0) {
            usleep(5 * 1000);
            continue;
        }

        char part[128];
        int pn = snprintf(part, sizeof(part),
                          "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                          len);
        if (send(fd, part, pn, 0) < 0 || send(fd, sbuf, len, 0) < 0 ||
            send(fd, "\r\n", 2, 0) < 0) {
            break;
        }
    }

    stream_viewers--;
    close(fd);
    return NULL;
}

static void http_file(int fd, const char *name)
{
    if (strstr(name, "..")) { http_send(fd, "403 Forbidden", "text/plain", (const uint8_t *)"no", 2); return; }
    char path[512];
    snprintf(path, sizeof(path), MEDIA_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { http_send(fd, "404 Not Found", "text/plain", (const uint8_t *)"not found", 9); return; }

    const char *ctype = "application/octet-stream";
    if (strstr(name, ".jpg")) ctype = "image/jpeg";
    else if (strstr(name, ".avi")) ctype = "video/x-msvideo";
    else if (strstr(name, ".json")) ctype = "application/json";

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n", ctype, size);
    send(fd, head, n, 0);

    uint8_t buf[32768];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) send(fd, buf, got, 0);
    fclose(f);
}

static void *http_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("http: bind failed: %s\n", strerror(errno));
        close(srv);
        return NULL;
    }
    listen(srv, 8);
    printf("http: serving %s on :8080\n", MEDIA_DIR);
    fflush(stdout);

    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        char req[1024];
        ssize_t got = recv(fd, req, sizeof(req) - 1, 0);
        if (got > 0) {
            req[got] = 0;
            char path[512] = "/";
            if (sscanf(req, "GET %511s", path) == 1) {
                if (strncmp(path, "/stream.mjpg", 12) == 0) {
                    if (stream_viewers >= 3) {
                        http_send(fd, "503 Service Unavailable", "text/plain",
                                  (const uint8_t *)"busy", 4);
                    } else {
                        stream_viewers++;
                        pthread_t th;
                        if (pthread_create(&th, NULL, mjpeg_stream_thread,
                                           (void *)(intptr_t)fd) == 0) {
                            pthread_detach(th);
                            fd = -1;          /* owned by the stream thread */
                        } else {
                            stream_viewers--;
                        }
                    }
                } else if (strcmp(path, "/") == 0) {
                    http_listing(fd);
                } else if (strncmp(path, "/preview.jpg", 12) == 0) {
                    const uint8_t *jpg = NULL;
                    size_t len = 0;
                    if (preview_jpeg(&jpg, &len) == 0) {
                        http_send(fd, "200 OK", "image/jpeg", jpg, len);
                    } else {
                        http_send(fd, "503 Service Unavailable", "text/plain",
                                  (const uint8_t *)"no frame", 8);
                    }
                } else if (strncmp(path, "/slide", 6) == 0) {
                    const char *qd = strstr(path, "d=");
                    slide_req = qd ? atoi(qd + 2) : 1;
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"ok", 2);
                } else if (strncmp(path, "/preview", 8) == 0) {
                    const char *qf = strstr(path, "f=");
                    if (qf) {
                        char name[128];
                        snprintf(name, sizeof(name), "%s", qf + 2);
                        char *u = strdup(name);
                        preview_req = u;
                    }
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"ok", 2);
                } else if (strncmp(path, "/delete", 7) == 0) {
                    const char *qf = strstr(path, "f=");
                    int ok = 0;
                    if (qf) {
                        ok = (media_remove(qf + 2) == 0);
                        files_dirty = 1;
                    }
                    http_send(fd, "200 OK", "text/plain",
                              (const uint8_t *)(ok ? "deleted" : "failed"), ok ? 7 : 6);
                } else if (strncmp(path, "/poster", 7) == 0) {
                    const char *qf = strstr(path, "f=");
                    if (qf) http_poster(fd, qf + 2);
                    else http_send(fd, "400 Bad Request", "text/plain",
                                   (const uint8_t *)"no", 2);
                } else if (strncmp(path, "/zip", 4) == 0) {
                    static char znames[ZIP_MAX][128];
                    int zn = 0;
                    const char *p2 = path;
                    while ((p2 = strstr(p2, "f=")) != NULL && zn < ZIP_MAX) {
                        p2 += 2;
                        const char *end = strchr(p2, '&');
                        size_t len = end ? (size_t)(end - p2) : strlen(p2);
                        if (len >= sizeof(znames[0])) len = sizeof(znames[0]) - 1;
                        memcpy(znames[zn], p2, len);
                        znames[zn][len] = 0;
                        zn++;
                    }
                    http_zip(fd, znames, zn);
                } else if (strcmp(path, "/files.json") == 0) {
                    http_files_json(fd);
                } else if (strcmp(path, "/status") == 0) {
                    char js[160];
                    int n = snprintf(js, sizeof(js),
                                     "{\"recording\":%d,\"frames\":%d,\"files\":%d,\"focus\":%d}",
                                     recording ? 1 : 0, frame_count, g_file_count,
                                     (int)g_focus);
                    http_send(fd, "200 OK", "application/json", (const uint8_t *)js, n);
                } else if (strncmp(path, "/page", 5) == 0) {
                    const char *qp = strstr(path, "p=");
                    if (qp) page_req = atoi(qp + 2);
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"ok", 2);
                } else if (strncmp(path, "/wifi/modal", 11) == 0) {
                    modal_req = 1;
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"ok", 2);
                } else if (strcmp(path, "/wifi/scan") == 0) {
                    wifi_scan_async();
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"scanning", 8);
                } else if (strcmp(path, "/wifi/status") == 0) {
                    http_send(fd, "200 OK", "text/plain",
                              (const uint8_t *)g_status_text, strlen(g_status_text));
                } else if (strncmp(path, "/screen", 7) == 0) {
                    const char *qb = strstr(path, "b=");
                    if (qb) screen_pending = atoi(qb + 2);
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"queued", 6);
                } else if (strncmp(path, "/color", 6) == 0) {
                    int b = bright_val, c = contrast_val;
                    const char *qb = strstr(path, "b=");
                    const char *qc = strstr(path, "c=");
                    if (qb) b = atoi(qb + 2);
                    if (qc) c = atoi(qc + 2);
                    if (b < 1) b = 1;
                    if (b > 100) b = 100;
                    if (c < 1) c = 1;
                    if (c > 100) c = 100;
                    bright_pending = b;
                    contrast_pending = c;
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"queued", 6);
                } else if (strcmp(path, "/capture") == 0) {
                    photo_capture();
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"captured", 8);
                } else if (strcmp(path, "/record/start") == 0) {
                    record_start();
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"recording", 9);
                } else if (strcmp(path, "/record/stop") == 0) {
                    record_stop();
                    http_send(fd, "200 OK", "text/plain", (const uint8_t *)"stopped", 7);
                } else if (strncmp(path, "/files/", 7) == 0) {
                    http_file(fd, path + 7);
                } else {
                    http_send(fd, "404 Not Found", "text/plain", (const uint8_t *)"no", 2);
                }
            }
        }
        if (fd >= 0) close(fd);
    }
    close(srv);
    return NULL;
}

/* ---------------- reverse proxy: camera web UI on :8081 ----------------
 * The camera's own web UI uses absolute paths (/ISAPI/...), so a path-prefix
 * proxy would break it. Serving a dedicated port that forwards everything
 * 1:1 keeps the UI fully functional from the WiFi side.
 */

struct proxy_buf {
    uint8_t *data;
    size_t len;
};

struct proxy_hdrs {
    char set_cookie[2048];
};

static size_t proxy_header_cb(char *buf, size_t size, size_t nmemb, void *userdata)
{
    struct proxy_hdrs *h = userdata;
    size_t len = size * nmemb;
    if (len > 11 && !strncasecmp(buf, "Set-Cookie:", 11)) {
        size_t used = strlen(h->set_cookie);
        if (used + len < sizeof(h->set_cookie)) {
            memcpy(h->set_cookie + used, buf, len);
            h->set_cookie[used + len] = 0;
        }
    }
    return len;
}

static size_t proxy_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct proxy_buf *b = userdata;
    size_t n = size * nmemb;
    uint8_t *p = realloc(b->data, b->len + n);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    return n;
}

#define CAMERA_IP "192.168.64.64"

struct relay_arg {
    int fd;
    int port;
};

static void *relay_connection(void *arg)
{
    struct relay_arg ra = *(struct relay_arg *)arg;
    free(arg);
    int cfd = ra.fd;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { close(cfd); return NULL; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)ra.port);
    if (inet_pton(AF_INET, CAMERA_IP, &a.sin_addr) != 1 ||
        connect(sfd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(cfd);
        close(sfd);
        return NULL;
    }

    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* plain byte pump: the browser talks HTTP/S with the camera end to end,
     * so sessions, cookies and chunked bodies all behave like a direct link */
    static __thread char buf[16384];
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(cfd, &rf);
        FD_SET(sfd, &rf);
        int mx = (cfd > sfd ? cfd : sfd) + 1;
        if (select(mx, &rf, NULL, NULL, NULL) <= 0) break;

        if (FD_ISSET(cfd, &rf)) {
            ssize_t n = recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(sfd, buf, (size_t)n, 0) != n) break;
        } else if (FD_ISSET(sfd, &rf)) {
            ssize_t n = recv(sfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(cfd, buf, (size_t)n, 0) != n) break;
        }
    }

    close(cfd);
    close(sfd);
    return NULL;
}

static void *proxy_thread(void *arg)
{
    int lport = (int)(intptr_t)arg;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)lport);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("relay: bind :%d failed: %s\n", lport, strerror(errno));
        close(srv);
        return NULL;
    }
    listen(srv, 8);
    printf("relay: :%d -> %s:%d\n", lport, CAMERA_IP, lport == 8081 ? 80 : lport);
    fflush(stdout);

    while (running) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        struct relay_arg *ra = malloc(sizeof(*ra));
        if (!ra) { close(fd); continue; }
        ra->fd = fd;
        ra->port = (lport == 8081) ? 80 : lport;
        pthread_t th;
        if (pthread_create(&th, NULL, relay_connection, ra) != 0) {
            free(ra);
            close(fd);
        } else {
            pthread_detach(th);
        }
    }
    close(srv);
    return NULL;
}

/* ---------------- UI ---------------- */

static lv_obj_t *mk_txt(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                        lv_color_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, int x, int y, int w, int h,
                        uint32_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_t *l = mk_txt(b, txt, FONT_CN, lv_color_hex(0xFFFFFF), 0, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

/* ---------------- spot measurement ---------------- */

static int spot_hit(int x, int y)
{
    int best = -1, best_d = 30 * 30;
    for (int i = 0; i < spot_count; i++) {
        int dx = spots[i].x - x, dy = spots[i].y - y;
        int d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void spot_place(int i, int x, int y, int in_build)
{
    if (x < 0) x = 0;
    if (x > VIEW_W - 1) x = VIEW_W - 1;
    if (y < 0) y = 0;
    if (y > VIEW_H - 1) y = VIEW_H - 1;
    spots[i].x = x;
    spots[i].y = y;
    if (!in_build && spots[i].dot) {
        lv_obj_set_pos(spots[i].dot, x - 9, y - 9);
        lv_obj_set_pos(spots[i].lbl, (x + 14 > VIEW_W - 60) ? x - 58 : x + 14, y + 6);
    }
}

static int spot_add(int x, int y)
{
    if (spot_count >= MAX_SPOTS) return -1;
    int i = spot_count++;
    spots[i].t = 0;
    spots[i].valid = 0;

    spots[i].dot = lv_obj_create(img_touch);
    lv_obj_remove_style_all(spots[i].dot);
    lv_obj_set_size(spots[i].dot, 18, 18);
    lv_obj_set_style_radius(spots[i].dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(spots[i].dot, lv_color_hex(0xFFA500), 0);
    lv_obj_set_style_bg_opa(spots[i].dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(spots[i].dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(spots[i].dot, 2, 0);
    lv_obj_remove_flag(spots[i].dot, LV_OBJ_FLAG_CLICKABLE);

    spots[i].lbl = lv_label_create(img_touch);
    lv_label_set_text(spots[i].lbl, "--.-");
    lv_obj_set_style_text_color(spots[i].lbl, lv_color_hex(0xFFD080), 0);
    lv_obj_set_style_text_font(spots[i].lbl, FONT_SM, 0);
    lv_obj_set_style_bg_color(spots[i].lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(spots[i].lbl, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(spots[i].lbl, 2, 0);
    lv_obj_remove_flag(spots[i].lbl, LV_OBJ_FLAG_CLICKABLE);

    spot_place(i, x, y, 0);
    printf("spot %d added at (%d,%d)\n", i, x, y);
    fflush(stdout);
    return i;
}

static void spot_remove(int i)
{
    if (i < 0 || i >= spot_count) return;
    lv_obj_delete(spots[i].dot);
    lv_obj_delete(spots[i].lbl);
    for (int k = i; k < spot_count - 1; k++) spots[k] = spots[k + 1];
    spot_count--;
    printf("spot %d removed, %d left\n", i, spot_count);
    fflush(stdout);
}

static void spot_clear_all(void);

static void img_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (p.x < 0) p.x = 0;
    if (p.x > VIEW_W - 1) p.x = VIEW_W - 1;
    if (p.y < 0) p.y = 0;
    if (p.y > VIEW_H - 1) p.y = VIEW_H - 1;

    if (code == LV_EVENT_PRESSED) {
        drag_idx = spot_hit(p.x, p.y);
        drag_moved = 0;
    } else if (code == LV_EVENT_PRESSING) {
        if (drag_idx >= 0) {
            drag_moved = 1;
            spot_place(drag_idx, p.x, p.y, 0);
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (drag_idx < 0 && !drag_moved) spot_add(p.x, p.y);
        drag_idx = -1;
        drag_moved = 0;
    } else if (code == LV_EVENT_RELEASED) {
        drag_idx = -1;
    }
}

static void spot_clear_all(void)
{
    while (spot_count > 0) spot_remove(spot_count - 1);
    printf("all spots cleared\n");
    fflush(stdout);
}

/* ---------------- markers / switches ---------------- */

static lv_obj_t *make_dot(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 14, 14);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(d, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(d, 2, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return d;
}

static lv_obj_t *make_tag(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, FONT_SM, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(l, 3, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static void style_switches(void)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(sw_btn[i],
                                  lv_color_hex(sw_on[i] ? 0x007ACC : 0x333333), 0);
        lv_obj_set_style_bg_opa(sw_btn[i], LV_OPA_COVER, 0);
    }
}

static void on_sw_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    sw_on[idx] = !sw_on[idx];
    style_switches();
}

static void on_clear_click(lv_event_t *e)
{
    (void)e;
    spot_clear_all();
}

static void on_photo_click(lv_event_t *e)
{
    (void)e;
    photo_capture();
}

static void on_rec_click(lv_event_t *e)
{
    (void)e;
    if (recording) record_stop();
    else record_start();
}

static void on_bright_change(lv_event_t *e)
{
    bright_pending = lv_slider_get_value(lv_event_get_target(e));
}

static void on_contrast_change(lv_event_t *e)
{
    contrast_pending = lv_slider_get_value(lv_event_get_target(e));
}

static void on_screen_change(lv_event_t *e)
{
    screen_pending = lv_slider_get_value(lv_event_get_target(e));
}

/* ---------------- WiFi (nmcli) ---------------- */

#define MAX_AP 24

struct wifi_ap {
    char ssid[64];
    int signal;
    char sec[24];
};

static struct wifi_ap g_aps[MAX_AP];
static int g_ap_count = 0;
static lv_obj_t *wifi_list;
static lv_obj_t *wifi_status_lbl;
static lv_obj_t *wifi_hint_lbl;
static lv_obj_t *wifi_modal;
static lv_obj_t *wifi_ta;
static lv_obj_t *wifi_kb;
static lv_obj_t *wifi_modal_title;
static char g_ssid_sel[64];
static char g_ip[64] = "";
static volatile int wifi_dirty = 0;
static volatile int wifi_connecting = 0;

static void shell_exec(const char *cmd, char *out, size_t out_n)
{
    FILE *f = popen(cmd, "r");
    if (!f) return;
    size_t used = 0;
    while (out && used + 1 < out_n) {
        size_t got = fread(out + used, 1, out_n - used - 1, f);
        if (got == 0) break;
        used += got;
    }
    if (out) out[used] = 0;
    pclose(f);
}

static void ip_refresh(void)
{
    /* prefer the WiFi address: usb0 (the camera link) is usually listed first */
    char buf[1024] = { 0 };
    shell_exec("ip -4 -o addr show 2>/dev/null", buf, sizeof(buf));

    char wifi[64] = "", other[64] = "";
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *inet = strstr(line, "inet ");
        if (!inet) continue;
        char ip[64] = { 0 };
        sscanf(inet + 5, "%63[0-9.]", ip);
        if (!ip[0] || strncmp(ip, "127.", 4) == 0) continue;
        if (!wifi[0] && strstr(line, " wlan")) snprintf(wifi, sizeof(wifi), "%s", ip);
        else if (!other[0]) snprintf(other, sizeof(other), "%s", ip);
    }
    if (wifi[0]) snprintf(g_ip, sizeof(g_ip), "%s", wifi);
    else if (other[0]) snprintf(g_ip, sizeof(g_ip), "%s", other);
    else snprintf(g_ip, sizeof(g_ip), "-");
}

static void wifi_scan(void)
{
    char buf[8192] = { 0 };
    shell_exec("nmcli -t -f SSID,SIGNAL,SECURITY device wifi list --rescan yes 2>/dev/null",
               buf, sizeof(buf));

    g_ap_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && g_ap_count < MAX_AP;
         line = strtok_r(NULL, "\n", &save)) {
        if (!line[0]) continue;
        char *p1 = strrchr(line, ':');          /* security */
        if (!p1) continue;
        *p1 = 0;
        char *p2 = strrchr(line, ':');          /* signal */
        if (!p2) continue;
        *p2 = 0;
        if (!line[0]) continue;
        /* skip duplicates */
        int dup = 0;
        for (int i = 0; i < g_ap_count; i++)
            if (strcmp(g_aps[i].ssid, line) == 0) dup = 1;
        if (dup) continue;
        snprintf(g_aps[g_ap_count].ssid, sizeof(g_aps[0].ssid), "%s", line);
        g_aps[g_ap_count].signal = atoi(p2 + 1);
        snprintf(g_aps[g_ap_count].sec, sizeof(g_aps[0].sec), "%s", p1 + 1);
        g_ap_count++;
    }
    printf("wifi: %d APs\n", g_ap_count);
    fflush(stdout);
}

/* runs on a worker thread: touches no LVGL object */
static void wifi_collect(void)
{
    char buf[256] = { 0 };
    shell_exec("nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null", buf, sizeof(buf));

    char ssid[64] = "";
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strstr(line, "wlan")) {
            char *c = strchr(line, ':');
            if (c) { *c = 0; snprintf(ssid, sizeof(ssid), "%s", line); }
            break;
        }
    }
    ip_refresh();
    if (ssid[0]) snprintf(g_status_text, sizeof(g_status_text), "WiFi: %s   %s", ssid, g_ip);
    else snprintf(g_status_text, sizeof(g_status_text), "WiFi: --   %s", g_ip);
}

static void wifi_status_refresh(void)
{
    if (wifi_status_lbl) lv_label_set_text(wifi_status_lbl, g_status_text);
}

static void on_wifi_ap_click(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    snprintf(g_ssid_sel, sizeof(g_ssid_sel), "%s", ssid);
    if (wifi_modal_title) lv_label_set_text(wifi_modal_title, g_ssid_sel);
    if (wifi_ta) lv_textarea_set_text(wifi_ta, "");
    if (wifi_modal) {
        lv_obj_clear_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(wifi_modal);
    }
}

static void *wifi_connect_thread(void *arg)
{
    char ssid[64];
    snprintf(ssid, sizeof(ssid), "%s", g_ssid_sel);
    char pw[128];
    snprintf(pw, sizeof(pw), "%s", (const char *)arg);
    free(arg);

    char cmd[320];
    if (pw[0])
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect \"%s\" password \"%s\" 2>&1", ssid, pw);
    else
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect \"%s\" 2>&1", ssid);

    char out[512] = { 0 };
    shell_exec(cmd, out, sizeof(out));
    printf("wifi connect [%s]: %s\n", ssid, out);
    fflush(stdout);

    wifi_connecting = 0;
    wifi_collect();
    wifi_dirty = 1;
    return NULL;
}

static void on_wifi_connect_click(lv_event_t *e)
{
    (void)e;
    if (wifi_connecting) return;
    const char *pw = wifi_ta ? lv_textarea_get_text(wifi_ta) : "";
    char *pwcopy = strdup(pw ? pw : "");
    if (wifi_modal) lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
    if (wifi_hint_lbl) lv_label_set_text(wifi_hint_lbl, "连接中");
    wifi_connecting = 1;
    pthread_t th;
    pthread_create(&th, NULL, wifi_connect_thread, pwcopy);
    pthread_detach(th);
}

static void on_wifi_cancel_click(lv_event_t *e)
{
    (void)e;
    if (wifi_modal) lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
}

static void *wifi_scan_thread(void *arg)
{
    (void)arg;
    wifi_scan();
    wifi_collect();
    wifi_dirty = 2;
    return NULL;
}

static void wifi_scan_async(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, wifi_scan_thread, NULL) == 0) pthread_detach(th);
}

static void on_wifi_refresh_click(lv_event_t *e)
{
    (void)e;
    if (wifi_hint_lbl) lv_label_set_text(wifi_hint_lbl, "扫描中");
    wifi_scan_async();
}

static void wifi_rebuild_list(void)
{
    if (!wifi_list) return;
    lv_obj_clean(wifi_list);
    for (int i = 0; i < g_ap_count; i++) {
        char label[128];
        snprintf(label, sizeof(label), "%s   %d%%%s",
                 g_aps[i].ssid, g_aps[i].signal,
                 g_aps[i].sec[0] ? "  *" : "");
        lv_obj_t *b = lv_button_create(wifi_list);
        lv_obj_set_width(b, 436);
        lv_obj_set_height(b, 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x263238), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_t *l = mk_txt(b, label, FONT_SM, lv_color_hex(0xDDDDDD), 0, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
        char *ssid = strdup(g_aps[i].ssid);
        lv_obj_add_event_cb(b, on_wifi_ap_click, LV_EVENT_CLICKED, ssid);
    }
}

/* ---------------- media files: list, preview, delete ---------------- */

static int g_file_count = 0;
static lv_obj_t *files_list;
static lv_obj_t *files_info;
static lv_obj_t *preview_modal, *preview_img, *preview_title, *preview_hint;
static lv_image_dsc_t preview_dsc;
static uint8_t *preview_buf;
static char g_preview_path[256];

static int cmp_mtime(const void *a, const void *b)
{
    const struct mfile *x = a, *y = b;
    return (y->mtime > x->mtime) - (y->mtime < x->mtime);
}

static void files_scan(void)
{
    g_file_count = 0;
    DIR *d = opendir(MEDIA_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_file_count < FILE_MAX) {
        if (e->d_name[0] == '.') continue;
        size_t n = strlen(e->d_name);
        int keep = (n > 4) && (!strcmp(e->d_name + n - 4, ".jpg") ||
                               !strcmp(e->d_name + n - 4, ".avi"));
        if (!keep) continue;
        char path[512];
        snprintf(path, sizeof(path), MEDIA_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        snprintf(g_files[g_file_count].name, sizeof(g_files[0].name), "%s", e->d_name);
        g_files[g_file_count].size = (long)st.st_size;
        g_files[g_file_count].mtime = st.st_mtime;
        g_file_count++;
    }
    closedir(d);
    qsort(g_files, g_file_count, sizeof(g_files[0]), cmp_mtime);
}

static int jpeg_to_rgb565(const char *path, uint8_t **out, int *ow, int *oh)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = cinfo.output_width, h = cinfo.output_height;
    uint8_t *buf = malloc((size_t)w * h * 2);
    uint8_t *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&cinfo, rp, 1);
        uint8_t *dst = buf + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(((row[x * 3] & 0xF8) << 8) |
                                    ((row[x * 3 + 1] & 0xFC) << 3) |
                                    (row[x * 3 + 2] >> 3));
            dst[x * 2] = v & 0xFF;
            dst[x * 2 + 1] = v >> 8;
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);

    *out = buf;
    *ow = w;
    *oh = h;
    return 0;
}

static void files_rebuild(void);

static int media_remove(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), MEDIA_DIR "/%s", name);
    int rc = (remove(path) == 0) ? 0 : -1;

    /* photos come with a .json sidecar */
    size_t n = strlen(name);
    if (n > 4 && !strcmp(name + n - 4, ".jpg")) {
        char json[512];
        snprintf(json, sizeof(json), MEDIA_DIR "/%.*s.json", (int)(n - 4), name);
        remove(json);
    }
    return rc;
}

static void on_file_delete(lv_event_t *e)
{
    char *name = (char *)lv_event_get_user_data(e);
    if (!name) return;
    if (media_remove(name) == 0) printf("deleted %s\n", name);
    else printf("delete failed: %s\n", name);
    fflush(stdout);
    free(name);
    files_scan();
    files_rebuild();
}

static void close_preview(void)
{
    video_preview_stop();
    if (preview_modal) lv_obj_add_flag(preview_modal, LV_OBJ_FLAG_HIDDEN);
    free(preview_buf);
    preview_buf = NULL;
}

static void on_preview_play(lv_event_t *e)
{
    (void)e;
    if (!vid_reader.f) return;
    vid_playing = !vid_playing;
    if (preview_play)
        lv_label_set_text(lv_obj_get_child(preview_play, 0),
                          vid_playing ? "\u6682\u505c" : "\u64ad\u653e");
}

static lv_obj_t *preview_swipe;
static int swipe_x0;

static void preview_step(int dir)
{
    if (g_file_count <= 0 || !g_preview_path[0]) return;
    const char *base = strrchr(g_preview_path, '/');
    const char *cur = base ? base + 1 : g_preview_path;

    int idx = -1;
    for (int i = 0; i < g_file_count; i++) {
        if (!strcmp(g_files[i].name, cur)) { idx = i; break; }
    }
    if (idx < 0) return;

    int n = g_file_count;
    int nidx = idx + dir;
    if (nidx < 0) nidx = n - 1;
    if (nidx >= n) nidx = 0;
    if (nidx == idx) return;

    preview_open(g_files[nidx].name);
}

static void preview_rest_pos(int *x, int *y)
{
    size_t n = strlen(g_preview_path);
    int is_jpg = (n > 4) && !strcmp(g_preview_path + n - 4, ".jpg");
    *x = is_jpg ? 0 : (480 - VID_W) / 2;
    *y = is_jpg ? 60 : 180;
}

static int slide_dir;
static int slide_target;

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void slide_done(lv_anim_t *a)
{
    (void)a;
    int rx = 0, ry = 0;
    preview_rest_pos(&rx, &ry);
    lv_obj_set_pos(preview_img, rx, ry);       /* land exactly on the rest position */
}

/* swap to the neighbouring file, then slide the new picture in from the side */
static void preview_slide(int dir)
{
    if (!preview_img || !preview_modal) return;
    if (lv_obj_has_flag(preview_modal, LV_OBJ_FLAG_HIDDEN)) return;

    slide_dir = dir;
    lv_anim_delete(preview_img, NULL);         /* kill a slide that is still running */
    preview_step(dir);

    int rx = 0, ry = 0;
    preview_rest_pos(&rx, &ry);
    lv_obj_set_pos(preview_img, rx, ry);
    slide_target = rx;

    int from = (dir > 0) ? 480 : -480;
    lv_obj_set_x(preview_img, from);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, preview_img);
    lv_anim_set_values(&a, from, slide_target);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_ready_cb(&a, slide_done);
    lv_anim_start(&a);

}

static void on_preview_swipe(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        swipe_x0 = p.x;
    } else if (code == LV_EVENT_RELEASED) {
        int dx = p.x - swipe_x0;
        if (dx > 80) preview_slide(-1);
        else if (dx < -80) preview_slide(1);
    }
}

static void on_preview_close(lv_event_t *e)
{
    (void)e;
    close_preview();
}

static void on_preview_delete(lv_event_t *e)
{
    (void)e;
    if (g_preview_path[0]) {
        const char *base = strrchr(g_preview_path, '/');
        media_remove(base ? base + 1 : g_preview_path);
        printf("deleted %s\n", g_preview_path);
        fflush(stdout);
    }
    close_preview();
    files_scan();
    files_rebuild();
}

static void preview_open(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), MEDIA_DIR "/%s", name);
    snprintf(g_preview_path, sizeof(g_preview_path), "%s", path);

    free(preview_buf);
    preview_buf = NULL;

    size_t n = strlen(name);
    int is_jpg = (n > 4) && !strcmp(name + n - 4, ".jpg");

    if (is_jpg) {
        int w = 0, h = 0;
        if (jpeg_to_rgb565(path, &preview_buf, &w, &h) == 0) {
            preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            preview_dsc.header.w = w;
            preview_dsc.header.h = h;
            preview_dsc.header.stride = w * 2;
            preview_dsc.data_size = (uint32_t)w * h * 2;
            preview_dsc.data = preview_buf;
            lv_image_set_src(preview_img, &preview_dsc);
            lv_obj_set_size(preview_img, w, h);
            lv_obj_set_pos(preview_img, 0, 60);
            lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
            if (preview_hint) lv_obj_add_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);
            if (preview_play) lv_obj_add_flag(preview_play, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        video_preview_stop();
        if (avi_open_reader(path, &vid_reader) == 0) {
            vid_stop = 0;
            vid_playing = 1;
            if (pthread_create(&vid_tid, NULL, video_preview_thread, NULL) == 0)
                vid_thread_live = 1;

            preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            preview_dsc.header.w = VID_W;
            preview_dsc.header.h = VID_H;
            preview_dsc.header.stride = VID_W * 2;
            preview_dsc.data_size = sizeof(vid_show);
            preview_dsc.data = vid_show;
            lv_image_set_src(preview_img, &preview_dsc);
            lv_obj_set_size(preview_img, VID_W, VID_H);
            lv_obj_set_pos(preview_img, (480 - VID_W) / 2, 180);
            lv_obj_clear_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
            if (preview_hint) lv_obj_add_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);
            if (preview_play) {
                lv_obj_clear_flag(preview_play, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(lv_obj_get_child(preview_play, 0), "暂停");
            }
        } else {
            lv_obj_add_flag(preview_img, LV_OBJ_FLAG_HIDDEN);
            if (preview_hint) lv_obj_clear_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);
            if (preview_play) lv_obj_add_flag(preview_play, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (preview_title) lv_label_set_text(preview_title, name);
    if (preview_modal) {
        lv_obj_clear_flag(preview_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(preview_modal);
    }
}

static void on_file_open(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    preview_open(name);
}

/* ---------------- photo thumbnails (phone-gallery look) ---------------- */

#define TW 140
#define TH 105
#define THUMB_MAX 60

struct thumb {
    uint8_t buf[TW * TH * 2];
    int ready;
    char name[128];
};

static struct thumb g_thumbs[THUMB_MAX];
static volatile int file_filter = 0;   /* 0=all 1=photo 2=video */
static lv_obj_t *filter_btn[3];
static volatile int thumbs_busy = 0;

static void thumb_from_rgb565(const uint8_t *src, int sw, int sh, uint8_t *dst)
{
    for (int y = 0; y < TH; y++) {
        int sy = y * sh / TH;
        const uint8_t *srow = src + (size_t)sy * sw * 2;
        uint8_t *drow = dst + (size_t)y * TW * 2;
        for (int x = 0; x < TW; x++) {
            int sx = x * sw / TW;
            drow[x * 2] = srow[sx * 2];
            drow[x * 2 + 1] = srow[sx * 2 + 1];
        }
    }
}

static void *thumbs_thread(void *arg)
{
    (void)arg;
    int n = g_file_count < THUMB_MAX ? g_file_count : THUMB_MAX;
    static uint8_t full[VIEW_W * VIEW_H * 2];

    for (int i = 0; i < n; i++) {
        snprintf(g_thumbs[i].name, sizeof(g_thumbs[i].name), "%s", g_files[i].name);
        g_thumbs[i].ready = 0;

        char path[512];
        snprintf(path, sizeof(path), MEDIA_DIR "/%s", g_files[i].name);
        size_t len = strlen(g_files[i].name);
        int is_jpg = (len > 4) && !strcmp(g_files[i].name + len - 4, ".jpg");

        int w = 0, h = 0, ok = 0;
        if (is_jpg) {
            uint8_t *buf = NULL;
            if (jpeg_to_rgb565(path, &buf, &w, &h) == 0) {
                thumb_from_rgb565(buf, w, h, g_thumbs[i].buf);
                free(buf);
                ok = 1;
            }
        } else {
            struct avi_reader r;
            if (avi_open_reader(path, &r) == 0) {
                uint8_t *jpg = NULL;
                size_t jl = 0;
                if (avi_next_jpeg(&r, &jpg, &jl) == 0) {
                    if (jpeg_mem_to_rgb565(jpg, jl, full, &w, &h) == 0 && w > 0 && h > 0) {
                        thumb_from_rgb565(full, w, h, g_thumbs[i].buf);
                        ok = 1;
                    }
                    free(jpg);
                }
                avi_close_reader(&r);
            }
        }
        g_thumbs[i].ready = ok;
    }
    thumbs_busy = 0;
    files_dirty = 1;               /* repaint the grid with real thumbnails */
    return NULL;
}

static void thumbs_start(void)
{
    if (thumbs_busy) return;
    thumbs_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, thumbs_thread, NULL) == 0) pthread_detach(th);
    else thumbs_busy = 0;
}

static void files_rebuild(void)
{
    if (!files_list) return;
    lv_obj_clean(files_list);

    long total = 0;
    for (int i = 0; i < g_file_count; i++) total += g_files[i].size;
    char info[96];
    snprintf(info, sizeof(info), "%d \u4e2a\u6587\u4ef6  \u5171 %.1f MB", g_file_count,
             total / 1048576.0);
    if (files_info) lv_label_set_text(files_info, info);

    int n = g_file_count < THUMB_MAX ? g_file_count : THUMB_MAX;
    int shown = 0;
    for (int i = 0; i < n; i++) {
        {
            size_t l2 = strlen(g_files[i].name);
            int vid = (l2 > 4) && !strcmp(g_files[i].name + l2 - 4, ".avi");
            if (file_filter == 1 && vid) continue;
            if (file_filter == 2 && !vid) continue;
        }
        /* cell */
        lv_obj_t *cell = lv_obj_create(files_list);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 148, 142);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_file_open, LV_EVENT_CLICKED, strdup(g_files[i].name));

        lv_obj_t *img = lv_image_create(cell);
        lv_obj_set_size(img, TW, TH);
        lv_obj_set_pos(img, 4, 4);
        size_t nm_len = strlen(g_files[i].name);
        int is_vid = (nm_len > 4) && !strcmp(g_files[i].name + nm_len - 4, ".avi");
        lv_obj_set_style_border_color(img, lv_color_hex(is_vid ? 0xD9A441 : 0x2F343B), 0);
        lv_obj_set_style_border_width(img, 2, 0);

        if (i < THUMB_MAX && g_thumbs[i].ready &&
            !strcmp(g_thumbs[i].name, g_files[i].name)) {
            /* a lv_image can point straight at the cached RGB565 buffer */
            static lv_image_dsc_t dscs[THUMB_MAX];
            dscs[i].header.magic = LV_IMAGE_HEADER_MAGIC;
            dscs[i].header.cf = LV_COLOR_FORMAT_RGB565;
            dscs[i].header.w = TW;
            dscs[i].header.h = TH;
            dscs[i].header.stride = TW * 2;
            dscs[i].data_size = TW * TH * 2;
            dscs[i].data = g_thumbs[i].buf;
            lv_image_set_src(img, &dscs[i]);
        } else {
            lv_obj_set_style_bg_color(img, lv_color_hex(0x1B2026), 0);
            lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        }

        /* file time under the thumbnail */
        char ts[32] = "";
        size_t ln = strlen(g_files[i].name);
        /* photo_YYYYmmdd_HHMMSS.jpg: YYYY=6..9 MM=10..11 DD=12..13 hh=15..16 mm=17..18 */
        if (ln > 21) {
            snprintf(ts, sizeof(ts), "%.2s-%.2s %.2s:%.2s",
                     g_files[i].name + 10, g_files[i].name + 12,
                     g_files[i].name + 15, g_files[i].name + 17);
        } else {
            snprintf(ts, sizeof(ts), "%s", g_files[i].name);
        }
        mk_txt(cell, ts, FONT_SM, lv_color_hex(0xBBBBBB), 6, 114);

        if (is_vid) {
            lv_obj_t *badge = lv_obj_create(cell);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, 56, 26);
            lv_obj_set_pos(badge, 6, 6);
            lv_obj_set_style_bg_color(badge, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_60, 0);
            lv_obj_set_style_radius(badge, 4, 0);
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            mk_txt(badge, "视频", FONT_SM, lv_color_hex(0xFFD080), 8, 2);
        }
        shown++;
    }

}

static void style_filters(void)
{
    for (int i = 0; i < 3; i++) {
        if (!filter_btn[i]) continue;
        lv_obj_set_style_bg_color(filter_btn[i],
                                  lv_color_hex(i == file_filter ? 0x007ACC : 0x2A2F35), 0);
    }
}

static void on_filter_click(lv_event_t *e)
{
    file_filter = (int)(intptr_t)lv_event_get_user_data(e);
    style_filters();
    files_rebuild();
}

static void on_files_refresh(lv_event_t *e)
{
    (void)e;
    files_scan();
    files_rebuild();
    thumbs_start();
}

/* ---------------- pages ---------------- */

static lv_obj_t *page_thermal, *page_settings, *page_wifi, *page_files;
static lv_obj_t *tab_btn[4];
static int cur_page = 0;
static lv_obj_t *url_lbl, *url_lbl2;

static void show_page(int idx)
{
    cur_page = idx;
    lv_obj_t *pages[4] = { page_thermal, page_settings, page_wifi, page_files };
    for (int i = 0; i < 4; i++) {
        if (!pages[i]) continue;
        if (i == idx) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (idx == 3 && files_list) {
        files_scan();
        files_rebuild();
        thumbs_start();
    }
    for (int i = 0; i < 4; i++) {
        if (!tab_btn[i]) continue;
        lv_obj_set_style_bg_color(tab_btn[i],
                                  lv_color_hex(i == idx ? 0x007ACC : 0x2A2F35), 0);
    }
}

static void on_tab_click(lv_event_t *e)
{
    show_page((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *new_page(lv_obj_t *scr)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 480, 736);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* ---------------- page: thermal ---------------- */

static void build_page_thermal(lv_obj_t *scr)
{
    page_thermal = new_page(scr);

    img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc.header.w = VIEW_W;
    img_dsc.header.h = VIEW_H;
    img_dsc.header.stride = VIEW_W * 2;
    img_dsc.data_size = sizeof(frame_show);
    img_dsc.data = frame_show;

    img_obj = lv_image_create(page_thermal);
    lv_image_set_src(img_obj, &img_dsc);
    lv_obj_set_pos(img_obj, 0, 0);

    img_touch = lv_obj_create(page_thermal);
    lv_obj_remove_style_all(img_touch);
    lv_obj_set_size(img_touch, VIEW_W, VIEW_H);
    lv_obj_set_pos(img_touch, 0, 0);
    lv_obj_add_flag(img_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img_touch, img_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(img_touch, img_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(img_touch, img_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(img_touch, img_event_cb, LV_EVENT_RELEASED, NULL);

    dot_max = make_dot(page_thermal, lv_color_hex(0xFF4444));
    dot_min = make_dot(page_thermal, lv_color_hex(0x44A0FF));
    dot_ctr = make_dot(page_thermal, lv_color_hex(0x44FF88));
    tag_max = make_tag(page_thermal, lv_color_hex(0xFF8080));
    tag_min = make_tag(page_thermal, lv_color_hex(0x90C8FF));
    tag_ctr = make_tag(page_thermal, lv_color_hex(0x90FFB0));

    const char *sw_names[3] = { "最高", "最低", "中心" };
    for (int i = 0; i < 3; i++) {
        sw_btn[i] = lv_button_create(page_thermal);
        lv_obj_set_size(sw_btn[i], 144, 52);
        lv_obj_set_pos(sw_btn[i], 12 + i * 156, 368);
        lv_obj_set_style_radius(sw_btn[i], 10, 0);
        lv_obj_add_event_cb(sw_btn[i], on_sw_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        sw_lbl[i] = mk_txt(sw_btn[i], sw_names[i], FONT_CN, lv_color_hex(0xFFFFFF), 0, 0);
        lv_obj_center(sw_lbl[i]);
    }
    style_switches();

    btn_pal = mk_btn(page_thermal, PAL_CN[pal_current], 12, 428, 300, 52, 0x2D6A4F,
                     on_pal_click, NULL);
    lbl_pal = lv_obj_get_child(btn_pal, 0);
    mk_btn(page_thermal, "清除", 324, 428, 144, 52, 0x8B2E2E, on_clear_click, NULL);

    btn_photo = mk_btn(page_thermal, "拍照", 12, 488, 220, 52, 0x2D5F8B, on_photo_click, NULL);
    lv_obj_t *btn_rec = mk_btn(page_thermal, "录像", 248, 488, 220, 52, 0x2D6A4F,
                               on_rec_click, NULL);
    lbl_rec = lv_obj_get_child(btn_rec, 0);

    focus_track = lv_obj_create(page_thermal);
    lv_obj_remove_style_all(focus_track);
    lv_obj_set_size(focus_track, 456, 16);
    lv_obj_set_pos(focus_track, 12, 556);
    lv_obj_set_style_bg_color(focus_track, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(focus_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(focus_track, 8, 0);
    lv_obj_remove_flag(focus_track, LV_OBJ_FLAG_CLICKABLE);

    focus_fill = lv_obj_create(focus_track);
    lv_obj_remove_style_all(focus_fill);
    lv_obj_set_size(focus_fill, 0, 16);
    lv_obj_set_style_bg_color(focus_fill, lv_color_hex(0xD9534F), 0);
    lv_obj_set_style_bg_opa(focus_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(focus_fill, 8, 0);
    lv_obj_remove_flag(focus_fill, LV_OBJ_FLAG_CLICKABLE);

    focus_peak = lv_obj_create(focus_track);
    lv_obj_remove_style_all(focus_peak);
    lv_obj_set_size(focus_peak, 3, 16);
    lv_obj_set_pos(focus_peak, 456 * 85 / 100, 0);
    lv_obj_set_style_bg_color(focus_peak, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(focus_peak, LV_OPA_COVER, 0);
    lv_obj_remove_flag(focus_peak, LV_OBJ_FLAG_CLICKABLE);

    lbl_stat = mk_txt(page_thermal, "", FONT_SM, lv_color_hex(0x888888), 12, 584);
    lbl_hint = mk_txt(page_thermal, "点画面测温  拖动移动  清除按钮删除",
                      FONT_CN, lv_color_hex(0x666666), 12, 616);

    mk_txt(page_thermal, "网页", FONT_CN, lv_color_hex(0x555555), 12, 652);
    url_lbl2 = mk_txt(page_thermal, "http://--:8080", FONT_CN,
                      lv_color_hex(0x3A8DBF), 78, 652);
}

/* ---------------- page: settings ---------------- */

static void build_page_settings(lv_obj_t *scr)
{
    page_settings = new_page(scr);

    /* live preview on top, centred */
    img_preview_sm = lv_image_create(page_settings);
    lv_image_set_src(img_preview_sm, &img_small_dsc);
    lv_obj_set_size(img_preview_sm, SMALL_W, SMALL_H);
    lv_obj_set_pos(img_preview_sm, (480 - SMALL_W) / 2, 8);
    lv_obj_set_style_border_color(img_preview_sm, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(img_preview_sm, 2, 0);
    lv_obj_set_style_radius(img_preview_sm, 6, 0);

    mk_txt(page_settings, "图像", FONT_CN, lv_color_hex(0x007ACC), 12, 258);

    mk_txt(page_settings, "亮度", FONT_CN, lv_color_hex(0xCCCCCC), 12, 294);
    sl_bright = lv_slider_create(page_settings);
    lv_obj_set_size(sl_bright, 340, 16);
    lv_obj_set_pos(sl_bright, 108, 298);
    lv_slider_set_range(sl_bright, 1, 100);
    lv_slider_set_value(sl_bright, 45, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl_bright, on_bright_change, LV_EVENT_VALUE_CHANGED, NULL);

    mk_txt(page_settings, "对比", FONT_CN, lv_color_hex(0xCCCCCC), 12, 344);
    sl_contrast = lv_slider_create(page_settings);
    lv_obj_set_size(sl_contrast, 340, 16);
    lv_obj_set_pos(sl_contrast, 108, 348);
    lv_slider_set_range(sl_contrast, 1, 100);
    lv_slider_set_value(sl_contrast, 45, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl_contrast, on_contrast_change, LV_EVENT_VALUE_CHANGED, NULL);

    mk_txt(page_settings, "屏幕", FONT_CN, lv_color_hex(0xCCCCCC), 12, 394);
    sl_screen = lv_slider_create(page_settings);
    lv_obj_set_size(sl_screen, 340, 16);
    lv_obj_set_pos(sl_screen, 108, 398);
    lv_slider_set_range(sl_screen, 1, 100);
    lv_slider_set_value(sl_screen, screen_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl_screen, on_screen_change, LV_EVENT_VALUE_CHANGED, NULL);

    mk_txt(page_settings, "网页", FONT_CN, lv_color_hex(0x007ACC), 12, 450);
    url_lbl = mk_txt(page_settings, "http://--:8080", FONT_CN,
                     lv_color_hex(0x4FC1FF), 12, 484);

    mk_txt(page_settings, "测温点", FONT_CN, lv_color_hex(0x007ACC), 12, 540);
    mk_btn(page_settings, "全部清除", 12, 574, 456, 52, 0x8B2E2E, on_clear_click, NULL);

    mk_txt(page_settings, "4117 手持热像仪", FONT_CN,
           lv_color_hex(0x666666), 12, 700);
}

/* ---------------- page: wifi ---------------- */

static void build_page_wifi(lv_obj_t *scr)
{
    page_wifi = new_page(scr);

    wifi_status_lbl = mk_txt(page_wifi, "WiFi: --", FONT_SM,
                             lv_color_hex(0xCCCCCC), 12, 16);
    wifi_hint_lbl = mk_txt(page_wifi, "", FONT_CN, lv_color_hex(0x888888), 12, 48);

    mk_btn(page_wifi, "刷新", 324, 40, 144, 48, 0x007ACC, on_wifi_refresh_click, NULL);

    wifi_list = lv_obj_create(page_wifi);
    lv_obj_remove_style_all(wifi_list);
    lv_obj_set_size(wifi_list, 456, 600);
    lv_obj_set_pos(wifi_list, 12, 104);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list, 8, 0);
    lv_obj_set_scroll_dir(wifi_list, LV_DIR_VER);

}

static void build_wifi_modal(lv_obj_t *scr)
{
    /* full-screen dim layer */
    wifi_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(wifi_modal);
    lv_obj_set_size(wifi_modal, 480, 800);
    lv_obj_set_pos(wifi_modal, 0, 0);
    lv_obj_set_style_bg_color(wifi_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_modal, LV_OPA_80, 0);
    lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wifi_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_modal, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* card: a column of title / input / keyboard / buttons */
    lv_obj_t *card = lv_obj_create(wifi_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 464, 556);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2026), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    wifi_modal_title = mk_txt(card, "", FONT_SM, lv_color_hex(0xFFFFFF), 0, 0);

    wifi_ta = lv_textarea_create(card);
    lv_obj_set_size(wifi_ta, 430, 64);
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, true);
    lv_textarea_set_placeholder_text(wifi_ta, "password");

    wifi_kb = lv_keyboard_create(card);
    lv_obj_set_size(wifi_kb, 436, 330);
    lv_keyboard_set_textarea(wifi_kb, wifi_ta);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 436, 68);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mk_btn(row, "\u8fde\u63a5", 0, 0, 206, 64, 0x2D6A4F, on_wifi_connect_click, NULL);
    mk_btn(row, "\u53d6\u6d88", 0, 0, 206, 64, 0x444444, on_wifi_cancel_click, NULL);
}

/* ---------------- page: files ---------------- */

static void build_page_files(lv_obj_t *scr)
{
    page_files = new_page(scr);

    files_info = mk_txt(page_files, "", FONT_SM, lv_color_hex(0xCCCCCC), 12, 14);
    mk_btn(page_files, "\u5237\u65b0", 336, 4, 132, 44, 0x007ACC, on_files_refresh, NULL);

    filter_btn[0] = mk_btn(page_files, "\u5168\u90e8", 12, 56, 96, 40, 0x007ACC,
                           on_filter_click, (void *)0);
    filter_btn[1] = mk_btn(page_files, "\u7167\u7247", 116, 56, 96, 40, 0x2A2F35,
                           on_filter_click, (void *)1);
    filter_btn[2] = mk_btn(page_files, "\u89c6\u9891", 220, 56, 96, 40, 0x2A2F35,
                           on_filter_click, (void *)2);
    style_filters();

    files_list = lv_obj_create(page_files);
    lv_obj_remove_style_all(files_list);
    lv_obj_set_size(files_list, 456, 612);
    lv_obj_set_pos(files_list, 12, 110);
    lv_obj_add_flag(files_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(files_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(files_list, 6, 0);
    lv_obj_set_style_pad_column(files_list, 4, 0);
    lv_obj_set_scroll_dir(files_list, LV_DIR_VER);
}

static void build_preview_modal(lv_obj_t *scr)
{
    preview_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(preview_modal);
    lv_obj_set_size(preview_modal, 480, 800);
    lv_obj_set_pos(preview_modal, 0, 0);
    lv_obj_set_style_bg_color(preview_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(preview_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(preview_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_modal, LV_OBJ_FLAG_SCROLLABLE);

    preview_title = mk_txt(preview_modal, "", FONT_SM, lv_color_hex(0xCCCCCC), 12, 24);

    lv_obj_t *vid_bg = lv_obj_create(preview_modal);
    lv_obj_remove_style_all(vid_bg);
    lv_obj_set_size(vid_bg, 480, 280);
    lv_obj_set_pos(vid_bg, 0, 160);
    lv_obj_set_style_bg_color(vid_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(vid_bg, LV_OPA_COVER, 0);
    lv_obj_remove_flag(vid_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(vid_bg, LV_OBJ_FLAG_SCROLLABLE);

    preview_img = lv_image_create(preview_modal);
    lv_image_set_src(preview_img, &img_dsc);   /* valid src so the object has a real size */
    lv_obj_set_size(preview_img, VIEW_W, VIEW_H);
    lv_obj_set_pos(preview_img, 0, 60);

    preview_hint = mk_txt(preview_modal, "\u89c6\u9891\u6587\u4ef6\uff0c\u8bf7\u5728\u7f51\u9875\u4e0a\u67e5\u770b",
                          FONT_CN, lv_color_hex(0xAAAAAA), 12, 220);
    lv_obj_add_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);

    preview_swipe = lv_obj_create(preview_modal);
    lv_obj_remove_style_all(preview_swipe);
    lv_obj_set_size(preview_swipe, 480, 690);
    lv_obj_set_pos(preview_swipe, 0, 0);
    lv_obj_add_flag(preview_swipe, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(preview_swipe, on_preview_swipe, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(preview_swipe, on_preview_swipe, LV_EVENT_RELEASED, NULL);

    mk_txt(preview_modal, "\u5de6\u53f3\u6ed1\u52a8\u5207\u6362", FONT_CN,
           lv_color_hex(0x888888), 176, 636);

    mk_btn(preview_modal, "\u5220\u9664", 12, 700, 140, 64, 0x8B2E2E, on_preview_delete, NULL);
    preview_play = mk_btn(preview_modal, "\u64ad\u653e", 164, 700, 140, 64, 0x007ACC,
                          on_preview_play, NULL);
    mk_btn(preview_modal, "\u5173\u95ed", 316, 700, 152, 64, 0x444444, on_preview_close, NULL);
}

/* ---------------- build ---------------- */

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    img_small_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_small_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_small_dsc.header.w = SMALL_W;
    img_small_dsc.header.h = SMALL_H;
    img_small_dsc.header.stride = SMALL_W * 2;
    img_small_dsc.data_size = sizeof(img_small);
    img_small_dsc.data = img_small;

    build_page_thermal(scr);
    build_page_settings(scr);
    build_page_wifi(scr);
    build_page_files(scr);

    /* bottom tab bar */
    const char *tabs[4] = { "热像", "设置", "网络", "文件" };
    for (int i = 0; i < 4; i++) {
        tab_btn[i] = lv_button_create(scr);
        lv_obj_set_size(tab_btn[i], 108, 48);
        lv_obj_set_pos(tab_btn[i], 8 + i * 116, 744);
        lv_obj_set_style_radius(tab_btn[i], 10, 0);
        lv_obj_add_event_cb(tab_btn[i], on_tab_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_txt(tab_btn[i], tabs[i], FONT_CN, lv_color_hex(0xFFFFFF), 0, 0);
        lv_obj_center(l);
    }

    build_wifi_modal(scr);
    build_preview_modal(scr);
    files_scan();
    files_rebuild();
    show_page(0);
}

/* ---------------- update ---------------- */

static void update_ui(void)
{
    int modal_open = (wifi_modal && !lv_obj_has_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN)) ||
                     (preview_modal && !lv_obj_has_flag(preview_modal, LV_OBJ_FLAG_HIDDEN));
    if (frame_ready) {
        pthread_mutex_lock(&lock);
        memcpy(frame_show, frame_pending, sizeof(frame_show));
        frame_ready = 0;
        pthread_mutex_unlock(&lock);
        if (!modal_open) {
            lv_obj_invalidate(img_obj);
            if (img_preview_sm && cur_page == 1) {
                for (int y = 0; y < SMALL_H; y++) {
                    const uint8_t *src = frame_show + (size_t)(y * VIEW_H / SMALL_H) * VIEW_W * 2;
                    uint8_t *dst = img_small + (size_t)y * SMALL_W * 2;
                    for (int x = 0; x < SMALL_W; x++) {
                        int sx = x * VIEW_W / SMALL_W;
                        dst[x * 2] = src[sx * 2];
                        dst[x * 2 + 1] = src[sx * 2 + 1];
                    }
                }
                lv_obj_invalidate(img_preview_sm);
            }
        }
    }

    int version, cver;
    struct temps t;
    struct center_temp ct;
    pthread_mutex_lock(&lock);
    version = g_temps.version;
    t = g_temps;
    cver = g_center.version;
    ct = g_center;
    pthread_mutex_unlock(&lock);

    char buf[64];

    if (t.valid && version != shown_marker_ver) {
        int mx = (int)(t.max_x * VIEW_W) - 7, my = (int)(t.max_y * VIEW_H) - 7;
        int nx = (int)(t.min_x * VIEW_W) - 7, ny = (int)(t.min_y * VIEW_H) - 7;
        if (mx < 0) mx = 0;
        if (mx > VIEW_W - 14) mx = VIEW_W - 14;
        if (my < 0) my = 0;
        if (my > VIEW_H - 14) my = VIEW_H - 14;
        if (nx < 0) nx = 0;
        if (nx > VIEW_W - 14) nx = VIEW_W - 14;
        if (ny < 0) ny = 0;
        if (ny > VIEW_H - 14) ny = VIEW_H - 14;

        lv_obj_set_pos(dot_max, mx, my);
        snprintf(buf, sizeof(buf), "%.1f", t.max);
        lv_label_set_text(tag_max, buf);
        lv_obj_set_pos(tag_max, (mx + 18 > VIEW_W - 64) ? mx - 58 : mx + 18, my - 2);

        lv_obj_set_pos(dot_min, nx, ny);
        snprintf(buf, sizeof(buf), "%.1f", t.min);
        lv_label_set_text(tag_min, buf);
        lv_obj_set_pos(tag_min, (nx + 18 > VIEW_W - 64) ? nx - 58 : nx + 18, ny + 16);

        shown_marker_ver = version;
    }

    if (ct.valid && cver != shown_center_ver) {
        snprintf(buf, sizeof(buf), "%.1f", ct.value);
        lv_label_set_text(tag_ctr, buf);
        lv_obj_set_pos(dot_ctr, VIEW_W / 2 - 7, VIEW_H / 2 - 7);
        lv_obj_set_pos(tag_ctr, VIEW_W / 2 + 14, VIEW_H / 2 + 6);
        shown_center_ver = cver;
    }

    for (int i = 0; i < spot_count; i++) {
        if (spots[i].valid && spots[i].lbl) {
            snprintf(buf, sizeof(buf), "%.1f", spots[i].t);
            lv_label_set_text(spots[i].lbl, buf);
        }
    }

    if (sw_on[0]) { lv_obj_clear_flag(dot_max, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(tag_max, LV_OBJ_FLAG_HIDDEN); }
    else          { lv_obj_add_flag(dot_max, LV_OBJ_FLAG_HIDDEN);   lv_obj_add_flag(tag_max, LV_OBJ_FLAG_HIDDEN); }
    if (sw_on[1]) { lv_obj_clear_flag(dot_min, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(tag_min, LV_OBJ_FLAG_HIDDEN); }
    else          { lv_obj_add_flag(dot_min, LV_OBJ_FLAG_HIDDEN);   lv_obj_add_flag(tag_min, LV_OBJ_FLAG_HIDDEN); }
    if (sw_on[2]) { lv_obj_clear_flag(dot_ctr, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(tag_ctr, LV_OBJ_FLAG_HIDDEN); }
    else          { lv_obj_add_flag(dot_ctr, LV_OBJ_FLAG_HIDDEN);   lv_obj_add_flag(tag_ctr, LV_OBJ_FLAG_HIDDEN); }

    if (pal_applied >= 0) {
        int idx = pal_applied;
        pal_applied = -1;
        lv_label_set_text(lbl_pal, PAL_CN[idx]);
    }

    /* recording indicator */
    static int rec_shown = 0;
    if (recording != rec_shown) {
        rec_shown = recording;
        if (lbl_rec) lv_label_set_text(lbl_rec, recording ? "停止" : "录像");
    }

    /* debug/remote requests */
    if (page_req >= 0) {
        int p = page_req;
        page_req = -1;
        if (p >= 0 && p <= 3) show_page(p);
        if (wifi_modal) lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
    }
    if (modal_req) {
        modal_req = 0;
        if (wifi_modal) {
            if (wifi_modal_title) lv_label_set_text(wifi_modal_title, "OxO_Wi-Fi5");
            lv_obj_clear_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(wifi_modal);
        }
    }

    if (vid_ready && preview_img && !lv_obj_has_flag(preview_modal, LV_OBJ_FLAG_HIDDEN)) {
        static uint32_t last_draw;
        uint32_t now_ms = lv_tick_get();
        if (now_ms - last_draw >= 100) {       /* ~10 fps, keeps the redraw atomic */
            last_draw = now_ms;
            vid_ready = 0;
            pthread_mutex_lock(&lock);
            memcpy(vid_show, vid_tmp, sizeof(vid_show));
            pthread_mutex_unlock(&lock);
            lv_obj_invalidate(preview_img);
        }
    }

    if (files_dirty) {
        files_dirty = 0;
        files_scan();
        files_rebuild();
    }
    if (slide_req) {
        int d = slide_req;
        slide_req = 0;
        preview_slide(d > 0 ? 1 : -1);
    }
    if (preview_req) {
        char *name = preview_req;
        preview_req = NULL;
        preview_open(name);
        free(name);
    }

    /* wifi events */
    if (wifi_dirty) {
        int what = wifi_dirty;
        wifi_dirty = 0;
        if (what == 2) wifi_rebuild_list();
        wifi_status_refresh();
        char u[96];
        snprintf(u, sizeof(u), "http://%s:8080", g_ip[0] ? g_ip : "--");
        if (url_lbl) lv_label_set_text(url_lbl, u);
        if (url_lbl2) lv_label_set_text(url_lbl2, u);
        if (wifi_hint_lbl) lv_label_set_text(wifi_hint_lbl, "");
    }
    if (wifi_connecting && wifi_hint_lbl)
        lv_label_set_text(wifi_hint_lbl, "连接中");

    /* screen backlight */
    if (screen_pending >= 0) {
        int v = screen_pending;
        screen_pending = -1;
        screen_set(v);
    }

    if (color_synced) {
        color_synced = 0;
        if (sl_bright) lv_slider_set_value(sl_bright, bright_val, LV_ANIM_OFF);
        if (sl_contrast) lv_slider_set_value(sl_contrast, contrast_val, LV_ANIM_OFF);
    }

    /* focus bar */
    {
        float m = g_focus, p = g_focus_peak;
        int pct = p > 0.5f ? (int)(m * 100.0f / p) : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_obj_set_width(focus_fill, 456 * pct / 100);
        lv_obj_set_style_bg_color(focus_fill,
                                  lv_color_hex(pct >= 85 ? 0x3FA34D : (pct >= 50 ? 0xD9A441 : 0xD9534F)), 0);
    }

    static int last_count = -1;
    static time_t last_t = 0;
    static time_t last_log = 0;
    time_t now = time(NULL);
    if (now != last_t) {
        int fps = last_t ? (frame_count - last_count) : 0;
        snprintf(buf, sizeof(buf), "%d fps", fps);
        lv_label_set_text(lbl_stat, buf);
        last_count = frame_count;
        last_t = now;
    }

    if (now % 5 == 0 && now != last_log) {
        last_log = now;
        printf("[%ld] frames=%d max=%.1f min=%.1f ctr=%.1f focus=%.0f rec=%d page=%d\n",
               (long)now, frame_count, t.max, t.min,
               ct.valid ? ct.value : -999.0f, g_focus, recording, cur_page);
        fflush(stdout);
    }
}

static uint32_t lv_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

int main(void)
{
    printf("tb4117 verify: LVGL %d.%d.%d\n",
           lv_version_major(), lv_version_minor(), lv_version_patch());
    fflush(stdout);

    lv_init();
    lv_tick_set_cb(lv_tick_ms);      /* REQUIRED: otherwise every animation/timer is frozen */

    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        fprintf(stderr, "DRM display init failed\n");
        return 1;
    }
    /* REQUIRED: configure the DRM device + connector, otherwise the display stays
     * at LVGL's default 800x480 and nothing is ever committed to the panel. */
    lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);
    lv_display_set_default(disp);

    int disp_w = lv_display_get_horizontal_resolution(disp);
    int disp_h = lv_display_get_vertical_resolution(disp);
    printf("display: %dx%d\n", disp_w, disp_h);
    fflush(stdout);

    lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");

    screen_init();
    font_init();
    build_ui();
    media_init();
    screen_set(screen_pct);   /* apply the saved level */

    pthread_t tv, tt, tm, th;
    pthread_create(&tv, NULL, video_thread, NULL);
    pthread_create(&tt, NULL, temp_thread, NULL);
    pthread_create(&tm, NULL, matrix_thread, NULL);
    pthread_create(&th, NULL, http_thread, NULL);
    pthread_t tp, tp2, tp3;
    pthread_create(&tp, NULL, proxy_thread, (void *)(intptr_t)8081);   /* web UI  -> 80  */
    pthread_create(&tp2, NULL, proxy_thread, (void *)(intptr_t)554);   /* RTSP    -> 554 */
    pthread_create(&tp3, NULL, proxy_thread, (void *)(intptr_t)8000);  /* private -> 8000 */
    wifi_scan_async();
    pthread_t tc;
    pthread_create(&tc, NULL, color_thread, NULL);

    while (running) {
        uint32_t idle = lv_timer_handler();
        update_ui();
        usleep(2000);
        (void)idle;
    }
    return 0;
}
