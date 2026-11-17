/* dump the framebuffer currently scanned out, via atomic primary plane or legacy CRTC */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, (v >> 16) & 0xFFFF); }

static int save_fb(int fd, uint32_t fb_id, const char *out)
{
    drmModeFB *fb = drmModeGetFB(fd, fb_id);
    if (!fb) return -1;
    printf("fb %u  %ux%u pitch %u bpp %u\n", fb->fb_id, fb->width, fb->height, fb->pitch, fb->bpp);

    uint64_t size = (uint64_t)fb->pitch * fb->height;
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, fb->handle, &offset) != 0) {
        fprintf(stderr, "not a dumb buffer\n");
        drmModeFreeFB(fb);
        return -1;
    }
    uint8_t *map = mmap(0, size, PROT_READ, MAP_SHARED, fd, offset);
    if (map == MAP_FAILED) { perror("mmap"); drmModeFreeFB(fb); return -1; }

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); munmap(map, size); drmModeFreeFB(fb); return -1; }

    int w = fb->width, h = fb->height;
    int row = ((w * 3 + 3) / 4) * 4;
    unsigned datasize = (unsigned)(row * h);
    fputc('B', f); fputc('M', f);
    put32(f, 54 + datasize); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, w); put32(f, h);
    put16(f, 1); put16(f, 24); put32(f, 0); put32(f, datasize);
    put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);

    uint8_t *pad = calloc(1, row);
    long nonzero = 0;
    for (int y = h - 1; y >= 0; y--) {
        uint8_t *src = map + (uint64_t)y * fb->pitch;
        memset(pad, 0, row);
        if (fb->bpp == 32) {
            for (int x = 0; x < w; x++) {
                uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
                if (b | g | r) nonzero++;
                pad[x * 3 + 0] = b; pad[x * 3 + 1] = g; pad[x * 3 + 2] = r;
            }
        } else {
            for (int x = 0; x < w; x++) {
                unsigned v = src[x * 2] | (src[x * 2 + 1] << 8);
                unsigned r = ((v >> 11) & 0x1F) << 3, g = ((v >> 5) & 0x3F) << 2, b = (v & 0x1F) << 3;
                if (r | g | b) nonzero++;
                pad[x * 3 + 0] = b; pad[x * 3 + 1] = g; pad[x * 3 + 2] = r;
            }
        }
        fwrite(pad, 1, row, f);
    }
    fclose(f);
    free(pad);
    munmap(map, size);
    drmModeFreeFB(fb);
    printf("wrote %s (%dx%d, non-black %ld)\n", out, w, h, nonzero);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
    const char *out = argc > 2 ? argv[2] : "/tmp/fb.bmp";
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    uint32_t plane_fb = 0;
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0) {
        drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
        if (pres) {
            for (uint32_t i = 0; i < pres->count_planes; i++) {
                drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
                if (!p) continue;
                if (p->fb_id && p->crtc_id) {
                    printf("plane %u crtc %u fb %u\n", p->plane_id, p->crtc_id, p->fb_id);
                    if (!plane_fb) plane_fb = p->fb_id;
                }
                drmModeFreePlane(p);
            }
            drmModeFreePlaneResources(pres);
        }
    }
    if (plane_fb && save_fb(fd, plane_fb, out) == 0) { close(fd); return 0; }

    drmModeRes *res = drmModeGetResources(fd);
    for (int i = 0; res && i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
            drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
            if (e) {
                drmModeCrtc *crtc = drmModeGetCrtc(fd, e->crtc_id);
                if (crtc && crtc->buffer_id) {
                    printf("crtc %u fb %u mode %dx%d\n", crtc->crtc_id, crtc->buffer_id,
                           crtc->mode.hdisplay, crtc->mode.vdisplay);
                    int rc = save_fb(fd, crtc->buffer_id, out);
                    drmModeFreeCrtc(crtc);
                    drmModeFreeEncoder(e);
                    drmModeFreeConnector(c);
                    close(fd);
                    return rc;
                }
                if (crtc) drmModeFreeCrtc(crtc);
                drmModeFreeEncoder(e);
            }
        }
        drmModeFreeConnector(c);
    }
    fprintf(stderr, "no framebuffer found\n");
    return 1;
}
