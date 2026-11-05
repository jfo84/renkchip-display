/* bump.c — dongle receiver, M1 offline bench.
 * Read an H.264 ES file, decode through libvpu (VpuApiLegacy over libmpp),
 * blit each frame to the framebuffer. One-line summary at the end.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

#include "vpu_api.h"

#define ALIGN16(v) (((v) + 15) & ~15)

static unsigned char *fb_mem;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;

static int fb_open(const char *dev)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        close(fd);
        return -1;
    }
    if (vi.bits_per_pixel != 16) {
        fprintf(stderr, "stage:fb unexpected bpp %u\n", vi.bits_per_pixel);
        close(fd);
        return -1;
    }
    fb_mem = malloc(vi.xres * vi.yres * 2);
    if (!fb_mem) {
        close(fd);
        return -1;
    }
    fprintf(stderr, "stage:fb %ux%u virt %u bpp %u smem %u\n",
            vi.xres, vi.yres, vi.xres_virtual, vi.bits_per_pixel, fi.smem_len);
    return fd;
}

static unsigned short yuv_to_rgb565(int y, int u, int v)
{
    y -= 16;
    u -= 128;
    v -= 128;
    int r = (298 * y + 409 * v) >> 8;
    int g = (298 * y - 100 * u - 208 * v) >> 8;
    int b = (298 * y + 516 * u) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Blit one NV12 frame (src_w x src_h, given strides) to the fb, nearest fit. */
static void blit_nv12(int fd, const unsigned char *frame, int src_w, int src_h,
                      int hstride, int vstride)
{
    const unsigned char *y_plane = frame;
    const unsigned char *uv_plane = frame + hstride * vstride;
    int dst_w = vi.xres, dst_h = vi.yres;
    for (int dy = 0; dy < dst_h && dy < src_h; dy++) {
        int sy = dy * src_h / dst_h;
        unsigned short *dst = (unsigned short *)(fb_mem + dy * dst_w * 2);
        const unsigned char *yrow = y_plane + sy * hstride;
        const unsigned char *uvrow = uv_plane + (sy / 2) * hstride;
        for (int dx = 0; dx < dst_w && dx < src_w; dx++) {
            int sx = dx * src_w / dst_w;
            int y = yrow[sx];
            int u = uvrow[(sx & ~1)];
            int v = uvrow[(sx & ~1) + 1];
            dst[dx] = yuv_to_rgb565(y, u, v);
        }
    }
    lseek(fd, 0, SEEK_SET);
    write(fd, fb_mem, dst_w * dst_h * 2);
}

/* next AU boundary: start code before a slice NAL (type 1 or 5) */
static long next_au(const unsigned char *p, long size, long from)
{
    for (long i = from; i + 5 < size; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 &&
            ((p[i + 3] & 0x1F) == 1 || (p[i + 3] & 0x1F) == 5))
            return i;
    }
    return -1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* The mpp shim does not fill phy_addr/FrameBusAddr in the legacy structs
 * (they hold tiny fd-like numbers). Resolve the physical page ourselves. */
static int pm_fd = -1;

static unsigned int phys_of_fd(int pm, void *vaddr)
{
    unsigned long va = (unsigned long)vaddr;
    unsigned long long e = 0;
    if (pm < 0) return 0;
    if (lseek(pm, (va >> 12) * 8, SEEK_SET) < 0) return 0;
    if (read(pm, &e, 8) != 8) return 0;
    if (!(e >> 63 & 1)) return 0;  /* not resident */
    return (unsigned int)(((e & ((1ULL << 55) - 1)) << 12) | (va & 0xfff));
}

/* Uncached /dev/mem aliases for DMA-shared memory. The shim stubs every
 * cache op (VPUMemClean/Flush/Invalidate are 8-byte RETs), so cached
 * mappings read stale CPU lines instead of decoder writes and lose our
 * writes to the VOP. Map each page once; frames cycle a small pool. */
#define UC_MAX 8
static struct { unsigned int page; unsigned char *uc; } uc_map[UC_MAX];
static int uc_mem_fd = -1;

static unsigned char *uc_alias(void *vir, unsigned int len)
{
    unsigned int phy = phys_of_fd(pm_fd, vir);
    if (!phy) return NULL;
    unsigned int page = phy & ~0xfffu;
    if (uc_mem_fd < 0)
        uc_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (uc_mem_fd < 0) return NULL;
    for (int i = 0; i < UC_MAX; i++)
        if (uc_map[i].page == page && uc_map[i].uc)
            return uc_map[i].uc + (phy & 0xfffu);
    unsigned char *u = mmap(NULL, len + 0x1000, PROT_READ | PROT_WRITE,
                            MAP_SHARED, uc_mem_fd, page);
    if (u == MAP_FAILED) {
        fprintf(stderr, "uc_alias: mmap page=%08x failed: %s\n",
                page, strerror(errno));
        return NULL;
    }
    for (int i = 0; i < UC_MAX; i++) {
        if (!uc_map[i].uc) {
            uc_map[i].page = page;
            uc_map[i].uc = u;
            return u + (phy & 0xfffu);
        }
    }
    return u + (phy & 0xfffu);  /* no slot: leak this one mapping */
}

static unsigned int phys_of(void *vaddr)
{
    unsigned long va = (unsigned long)vaddr;
    unsigned long long e = 0;
    FILE *pm = fopen("/proc/self/pagemap", "rb");
    if (!pm) return 0;
    fseek(pm, (va >> 12) * 8, SEEK_SET);
    if (fread(&e, 8, 1, pm) != 1) e = 0;
    fclose(pm);
    if (!(e >> 63 & 1)) return 0;  /* not resident */
    return (unsigned int)(((e & ((1ULL << 55) - 1)) << 12) | (va & 0xfff));
}

/* Point LCDC win0 at a YUV420SP buffer given in bus addresses, and latch.
 * dsb around cfg_done: /dev/mem mappings are Normal-NC, a bare latch write
 * can overtake the register writes and arm a half-updated config. */
static int win0_program(unsigned int y_bus, unsigned int uv_bus, unsigned int vir)
{
    int mfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mfd < 0) return -1;
    unsigned char *lcdc = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, mfd, 0x10118000);
    if (lcdc == MAP_FAILED) { close(mfd); return -1; }
    unsigned int *sys_ctrl  = (unsigned int *)(lcdc + 0x00);
    unsigned int *dsp_ctrl0 = (unsigned int *)(lcdc + 0x04);
    unsigned int *yrgb_mst  = (unsigned int *)(lcdc + 0x20);
    unsigned int *cbr_mst   = (unsigned int *)(lcdc + 0x24);
    unsigned int *win_vir   = (unsigned int *)(lcdc + 0x30);
    unsigned int *cfg_done  = (unsigned int *)(lcdc + 0x90);
    *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (4u << 3);  /* win0 = YCBCR420 */
    *sys_ctrl &= ~(1u << 15);                          /* RB_SWAP off */
    *sys_ctrl |= 1u;                                   /* WIN0_EN */
    *yrgb_mst = y_bus;
    *cbr_mst  = uv_bus;
    *win_vir  = vir;
    *dsp_ctrl0 &= ~(3u << 20);                         /* CSC = BT.601 */
    *dsp_ctrl0 &= ~(1u << 14);                         /* progressive */
    __asm__ volatile("dsb sy" ::: "memory");
    *cfg_done = 0x01;
    __asm__ volatile("dsb sy" ::: "memory");
    fprintf(stderr, "stage:win0 y=%08x uv=%08x vir=%u sys=%08x\n",
            y_bus, uv_bus, vir, *sys_ctrl);
    munmap(lcdc, 4096);
    close(mfd);
    return 0;
}

int main(int argc, char **argv)
{
    pm_fd = open("/proc/self/pagemap", O_RDONLY);
    const char *es_path = argc > 1 ? argv[1] : "/mnt/ram0/wfdcap8.h264";
    const char *fb_dev = argc > 2 ? argv[2] : "/dev/graphics/fb1";
    int no_fb = strcmp(fb_dev, "none") == 0 || strcmp(fb_dev, "dump") == 0;
    int dump_mode = strcmp(fb_dev, "dump") == 0;
    int dump_index = argc > 3 ? atoi(argv[3]) : 1;

    if (strcmp(fb_dev, "regs") == 0) {
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        /* LCDC registers at 0x10118000 */
        unsigned char *regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mfd, 0x10118000);
        /* fb0 physical window at 0xc8845000, 8MB — stale address, may not map */
        unsigned char *fbp = mmap(NULL, 0x800000, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mfd, 0xc8845000);
        FILE *o = fopen("/mnt/ram0/regs.bin", "wb");
        if (o) {
            fwrite(regs, 1, 1024, o);
            fclose(o);
        }
        printf("bump: regs mapped %p fbphys %p", regs, fbp);
        for (int i = 0; i < 16; i++)
            printf(" r[%d]=%08x", i, *(unsigned int *)(regs + 4 * i));
        /* sample fb phys at the four candidate slots */
        if (fbp != MAP_FAILED) {
            for (int slot = 0; slot < 4; slot++) {
                unsigned int *p = (unsigned int *)(fbp + slot * 2097152);
                printf(" s%d=%08x/%08x", slot, p[0], p[64]);
            }
            munmap(fbp, 0x800000);
        } else {
            printf(" fbwin=MAP_FAILED");
        }
        munmap(regs, 4096);
        return 0;
    }

    if (strcmp(fb_dev, "probe") == 0) {
        /* Instrument the frame pool: dump one frame via the cached mapping
         * AND via per-page uncached aliases, plus the physical layout of the
         * first pages — the shim stubs cache ops, so the two views can
         * disagree and the pool need not be physically contiguous. */
        VpuCodecContext_t *pctx = NULL;
        if (vpu_open_context(&pctx) != 0 || !pctx) {
            printf("bump: probe open_ctx failed");
            return 1;
        }
        pctx->codecType = CODEC_DECODER;
        pctx->videoCoding = OMX_RK_VIDEO_CodingAVC;
        pctx->width = 1280;
        pctx->height = 720;
        pctx->enableparsing = 1;
        pctx->no_thread = 1;
        if (pctx->init(pctx, NULL, 0) != 0) {
            printf("bump: probe init failed");
            return 1;
        }
        FILE *pf = fopen(es_path, "rb");
        fseek(pf, 0, SEEK_END);
        long psz = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        unsigned char *pes = malloc(psz);
        if (fread(pes, 1, psz, pf) != (size_t)psz) return 1;
        fclose(pf);
        VPU_FRAME pv;
        DecoderOut_t pout;
        long poff = 0;
        int got = 0;
        while (poff < psz && !got) {
            long au = next_au(pes, psz, poff + 4);
            long chunk = (au > 0 ? au : psz) - poff;
            VideoPacket_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.data = pes + poff;
            pkt.size = chunk;
            pctx->decode_sendstream(pctx, &pkt);
            if (pkt.size == 0) poff += chunk; else usleep(2000);
            for (int r = 0; r < 50 && !got; r++) {
                memset(&pv, 0, sizeof(pv));
                memset(&pout, 0, sizeof(pout));
                pout.data = (unsigned char *)&pv;
                pctx->decode_getframe(pctx, &pout);
                if (pout.size && pv.vpumem.vir_addr) got = 1;
                else usleep(2000);
            }
        }
        if (!got) {
            printf("bump: probe got no frame");
            return 1;
        }
        unsigned char *v = (unsigned char *)pv.vpumem.vir_addr;
        fprintf(stderr, "probe: vir=%p fw=%u fh=%u\n", v,
                pv.FrameWidth, pv.FrameHeight);
        for (int i = 0; i < 8; i++) {
            unsigned char *up = uc_alias(v + i * 0x1000, 32);
            fprintf(stderr, "probe: page%d phys=%08x cached16=%02x%02x uc16=%02x%02x\n",
                    i, phys_of(v + i * 0x1000), v[i * 0x1000], v[i * 0x1000 + 1],
                    up ? up[0] : 0xee, up ? up[1] : 0xee);
        }
        FILE *c = fopen("/mnt/ram0/fr_cached.bin", "wb");
        if (c) { fwrite(v, 1, pv.FrameWidth * pv.FrameHeight * 3 / 2, c); fclose(c); }
        FILE *u = fopen("/mnt/ram0/fr_uc.bin", "wb");
        if (u) {
            /* page-by-page uncached read: no contiguity assumption */
            for (unsigned int o = 0;
                 o < pv.FrameWidth * pv.FrameHeight * 3 / 2; o += 0x1000) {
                unsigned int n = pv.FrameWidth * pv.FrameHeight * 3 / 2 - o;
                if (n > 0x1000) n = 0x1000;
                unsigned char *p8 = uc_alias(v + o, n);
                if (!p8) break;
                fwrite(p8, 1, n, u);
            }
            fclose(u);
        }
        VPUFreeLinear(&pv.vpumem);
        pctx->flush(pctx);
        vpu_close_context(&pctx);
        fprintf(stderr, "probe: done\n");
        return 0;
    }

    if (strcmp(fb_dev, "yuvtest") == 0) {
        /* Asymmetric pattern to disambiguate the win0 format:
         * Y=0xFF UV=0x00 → YUV420 read: extreme green full screen.
         * Same bytes as ARGB888: white top third, black below. */
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        unsigned char *wy = mmap(NULL, 1280 * 720, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, mfd, 0x60B88000);
        unsigned char *wuv = mmap(NULL, 1280 * 360, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mfd, 0x60C69000);
        unsigned char *lcdc = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mfd, 0x10118000);
        if (wy == MAP_FAILED || wuv == MAP_FAILED || lcdc == MAP_FAILED) {
            printf("bump: yuvtest mmap failed");
            return 1;
        }
        memset(wy, 0xFF, 1280 * 720);
        memset(wuv, 0x00, 1280 * 360);
        unsigned int *sys_ctrl = (unsigned int *)(lcdc + 0x00);
        unsigned int *dsp_ctrl0 = (unsigned int *)(lcdc + 0x04);
        unsigned int *cbr_mst = (unsigned int *)(lcdc + 0x24);
        unsigned int *cfg_done = (unsigned int *)(lcdc + 0x90);
        fprintf(stderr, "before sys=%08x dsp0=%08x cbr=%08x\n",
                *sys_ctrl, *dsp_ctrl0, *cbr_mst);
        *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (4u << 3);
        *sys_ctrl &= ~(1u << 15);
        *sys_ctrl |= 1u;
        *cbr_mst = 0x10c69000;
        *dsp_ctrl0 &= ~(3u << 20);
        *dsp_ctrl0 &= ~(1u << 14);
        *cfg_done = 0x01;
        fprintf(stderr, "after sys=%08x dsp0=%08x cbr=%08x — pattern up\n",
                *sys_ctrl, *dsp_ctrl0, *cbr_mst);
        sleep(90);
        return 0;
    }

    if (strcmp(fb_dev, "poke") == 0) {
        /* Re-latch win0 with explicit barriers: /dev/mem reg mappings can be
         * Normal-NC, where a bare cfg_done write can overtake the reg writes
         * and latch a half-updated config. dsb before AND after each latch. */
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        unsigned char *lcdc = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mfd, 0x10118000);
        if (lcdc == MAP_FAILED) {
            printf("bump: lcdc mmap failed");
            return 1;
        }
        unsigned int *sys_ctrl = (unsigned int *)(lcdc + 0x00);
        unsigned int *dsp_ctrl0 = (unsigned int *)(lcdc + 0x04);
        unsigned int *yrgb_mst = (unsigned int *)(lcdc + 0x20);
        unsigned int *cbr_mst = (unsigned int *)(lcdc + 0x24);
        unsigned int *win_vir = (unsigned int *)(lcdc + 0x30);
        unsigned int *cfg_done = (unsigned int *)(lcdc + 0x90);
        for (int i = 0; i < 6; i++) {
            *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (4u << 3);
            *sys_ctrl &= ~(1u << 15);
            *sys_ctrl |= 1u;
            *yrgb_mst = 0x10b88000;
            *cbr_mst = 0x10c69000;
            *win_vir = 1280;
            *dsp_ctrl0 &= ~(3u << 20);
            *dsp_ctrl0 &= ~(1u << 14);
            __asm__ volatile("dsb sy" ::: "memory");
            *cfg_done = 0x01;
            __asm__ volatile("dsb sy" ::: "memory");
            fprintf(stderr, "poke%d sys=%08x dsp0=%08x y=%08x cbr=%08x\n",
                    i, *sys_ctrl, *dsp_ctrl0, *yrgb_mst, *cbr_mst);
            usleep(200000);
        }
        munmap(lcdc, 4096);
        return 0;
    }

    if (strcmp(fb_dev, "peek") == 0) {
        unsigned long addr = argc > 3 ? strtoul(argv[3], NULL, 16) : 0x60B88000UL;
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        unsigned char *w = mmap(NULL, 1280 * 720 * 3 / 2, PROT_READ,
                                MAP_SHARED, mfd, addr);
        if (w == MAP_FAILED) {
            printf("bump: mmap %#lx failed: %s", addr, strerror(errno));
            return 1;
        }
        FILE *o = fopen("/mnt/ram0/win0.bin", "wb");
        if (o) {
            fwrite(w, 1, 1280 * 720 * 3 / 2, o);
            fclose(o);
        }
        printf("bump: peek %#lx ok, first words %08x %08x", addr,
               *(unsigned int *)w, *(unsigned int *)(w + 64));
        munmap(w, 1280 * 720 * 3 / 2);
        return 0;
    }

    /* phys mode: decode into a VPU-ALLOCATED scanout target and point LCDC
     * win0 (YUV420SP, hw-scaled to the 1080p panel) at it. Never write raw
     * physical memory: post-reboot 0x60b88000 is ramdisk/kernel-owned and
     * /dev/mem writes there corrupt files and panic the box. The decoder
     * reports each frame's Y/UV bus addresses (FrameBusAddr); win0 MSTs are
     * programmed in that same bus space.
     */
    static VPUMemLinear_t tgt;
    unsigned int tgt_phy = 0;           /* physical addr of the target */
    int win0_done = 0;
    unsigned char *tm = NULL;           /* target, UNCACHED /dev/mem alias */
    int phys_mode = strcmp(fb_dev, "phys") == 0;
    int pattern_mode = phys_mode && argc > 3 && strcmp(argv[3], "pattern") == 0;
    if (phys_mode) {
        if (VPUMallocLinear(&tgt, 1280 * 720 * 3 / 2) != 0 || !tgt.vir_addr) {
            printf("bump: VPUMallocLinear failed");
            return 1;
        }
        tgt_phy = phys_of(tgt.vir_addr);
        if (!tgt_phy) {
            printf("bump: pagemap resolve failed");
            return 1;
        }
        /* The shim stubs VPUMemClean/Flush (8-byte RETs), so writes through
         * the cached vir_addr never reach DRAM the VOP reads. Write the
         * target only through this uncached alias of our own allocation. */
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        tm = mmap(NULL, 1280 * 720 * 3 / 2, PROT_READ | PROT_WRITE,
                  MAP_SHARED, mfd, tgt_phy);
        if (tm == MAP_FAILED) {
            printf("bump: target mmap at %08x failed: %s", tgt_phy,
                   strerror(errno));
            return 1;
        }
        memset(tm, 0x10, 1280 * 720 * 3 / 2);  /* dark idle frame */
        no_fb = 0;
        fprintf(stderr, "stage:tgt phy=%08x shimphy=%08x vir=%p size=%u\n",
                tgt_phy, tgt.phy_addr, tgt.vir_addr, tgt.size);
    }

    FILE *f = fopen(es_path, "rb");
    if (!f) {
        printf("bump: open %s failed: %s", es_path, strerror(errno));
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long es_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *es = malloc(es_size);
    if (!es || fread(es, 1, es_size, f) != (size_t)es_size) {
        printf("bump: read %s failed", es_path);
        return 1;
    }
    fclose(f);

    int fbfd = -1;
    if (!no_fb && !phys_mode) {
        fbfd = fb_open(fb_dev);
        if (fbfd < 0) {
            printf("bump: fb %s failed: %s", fb_dev, strerror(errno));
            return 1;
        }
    }

    VpuCodecContext_t *ctx = NULL;
    fprintf(stderr, "stage:open_ctx\n");
    if (vpu_open_context(&ctx) != 0 || !ctx) {
        printf("bump: vpu_open_context failed");
        return 1;
    }
    fprintf(stderr, "stage:set_fields\n");
    ctx->codecType = CODEC_DECODER;
    ctx->videoCoding = OMX_RK_VIDEO_CodingAVC;
    ctx->width = 1280;
    ctx->height = 720;
    ctx->enableparsing = 1;
    ctx->no_thread = argc > 3 ? atoi(argv[3]) : 1;
    fprintf(stderr, "stage:init no_thread=%d\n", ctx->no_thread);
    if (ctx->init(ctx, NULL, 0) != 0) {
        printf("bump: ctx init failed");
        return 1;
    }
    fprintf(stderr, "stage:feed\n");

    int frames = 0;
    double dec_total = 0.0, dec_max = 0.0, dec_min = 1e9;
    int eos = 0;

    /* pull every available frame; blit if fb mode */
#define DRAIN(retries)                                                   \
    do {                                                                 \
        int empty = 0;                                                   \
        for (;;) {                                                       \
            VPU_FRAME vframe;                                            \
            DecoderOut_t out;                                            \
            memset(&vframe, 0, sizeof(vframe));                          \
            memset(&out, 0, sizeof(out));                                \
            out.data = (unsigned char *)&vframe;                         \
            double t0 = now_ms();                                        \
            int ret = ctx->decode_getframe(ctx, &out);                   \
            double dt = now_ms() - t0;                                   \
            if (out.size == 0 || !vframe.vpumem.vir_addr) {              \
                if (ret == VPU_API_EOS_STREAM_REACHED) { eos = 1; break; } \
                if (++empty > (retries)) break;                          \
                usleep(2000);                                            \
                continue;                                                \
            }                                                            \
            empty = 0;                                                   \
            dec_total += dt;                                             \
            if (dt > dec_max) dec_max = dt;                              \
            if (dt < dec_min) dec_min = dt;                              \
            frames++;                                                    \
            if (dump_mode && frames == dump_index) {                     \
                unsigned char *ds = uc_alias(vframe.vpumem.vir_addr,    \
                        vframe.FrameWidth * vframe.FrameHeight * 3 / 2);\
                if (!ds) ds = (unsigned char *)vframe.vpumem.vir_addr;  \
                FILE *df = fopen("/mnt/ram0/frame.bin", "wb");           \
                if (df) {                                                \
                    fwrite(ds, 1,                                        \
                           vframe.FrameWidth * vframe.FrameHeight * 3, df); \
                    fclose(df);                                          \
                }                                                        \
                fprintf(stderr, "stage:dumped %d fw=%u fh=%u\n",         \
                        frames, vframe.FrameWidth, vframe.FrameHeight);  \
                eos = 1;                                                 \
                break;                                                   \
            }                                                            \
            if (frames <= 3 || (frames & 31) == 0)                       \
                fprintf(stderr, "stage:frame%d %.1fms %ux%u err=%#x\n",  \
                        frames, dt, vframe.DisplayWidth, vframe.DisplayHeight, \
                        vframe.ErrorInfo);                               \
            if (phys_mode) {                                             \
                const unsigned char *src = uc_alias(vframe.vpumem.vir_addr, \
                        vframe.FrameWidth * vframe.FrameHeight * 3 / 2);\
                if (!src)                                                \
                    src = (unsigned char *)vframe.vpumem.vir_addr;       \
                int hs = vframe.FrameWidth, vs = vframe.FrameHeight;     \
                if (!win0_done) {                                        \
                    fprintf(stderr,                                      \
                            "stage:frame1 shimphy=%08x busy=%08x busuv=%08x\n", \
                            vframe.vpumem.phy_addr,                      \
                            vframe.FrameBusAddr[0],                      \
                            vframe.FrameBusAddr[1]);                     \
                    win0_program(tgt_phy - 0x50000000,                   \
                                 tgt_phy - 0x50000000 + hs * vs, hs);    \
                    win0_done = 1;                                       \
                }                                                        \
                if (pattern_mode) {                                      \
                    memset(tm, 0xFF, hs * vs);                           \
                    memset(tm + hs * vs, 0x00, hs * vs / 2);             \
                    eos = 1;                                             \
                    break;                                               \
                }                                                        \
                memcpy(tm, src, hs * vs);                                \
                memcpy(tm + hs * vs, src + hs * vs, hs * vs / 2);        \
                usleep(33000);                                           \
            } else if (!no_fb) {                                         \
                blit_nv12(fbfd, (unsigned char *)vframe.vpumem.vir_addr, \
                          vframe.DisplayWidth, vframe.DisplayHeight,     \
                          vframe.FrameWidth, vframe.FrameHeight);        \
                usleep(33000);                                           \
            }                                                            \
            VPUFreeLinear(&vframe.vpumem);  /* return the buffer */      \
        }                                                                \
    } while (0)

    long off = 0;
    while (off < es_size && !eos) {
        long au = next_au(es, es_size, off + 4);
        long chunk = (au > 0 ? au : es_size) - off;
        VideoPacket_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.data = es + off;
        pkt.size = chunk;
        double t0 = now_ms();
        ctx->decode_sendstream(ctx, &pkt);
        dec_total += now_ms() - t0;
        if (pkt.size == 0) {  /* accepted; pull what's ready */
            off += chunk;
            DRAIN(1);
        } else {
            DRAIN(10);        /* queue full: free buffers, retry chunk */
        }
    }
    /* EOS + final drain */
    {
        VideoPacket_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.size = 0;
        pkt.nFlags = 1; /* OMX_BUFFERFLAG_EOS */
        ctx->decode_sendstream(ctx, &pkt);
        DRAIN(500);
    }
    ctx->flush(ctx);
    fprintf(stderr, "stage:decoder_err=%d eos=%d\n", ctx->decoder_err, eos);
    vpu_close_context(&ctx);

    if (pattern_mode) {
        fprintf(stderr, "stage:pattern holding 90s\n");
        sleep(90);
    }
    if (phys_mode) {
        /* restore boot win0 (ARGB@0x10b88000) before freeing the target so
         * the VOP never scans freed/reused memory */
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        unsigned char *lcdc = mfd >= 0
            ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0x10118000)
            : MAP_FAILED;
        if (lcdc != MAP_FAILED) {
            unsigned int *sys_ctrl = (unsigned int *)(lcdc + 0x00);
            unsigned int *cbr_mst = (unsigned int *)(lcdc + 0x24);
            unsigned int *cfg_done = (unsigned int *)(lcdc + 0x90);
            *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (1u << 15);
            *cbr_mst = 0;
            __asm__ volatile("dsb sy" ::: "memory");
            *cfg_done = 0x01;
            __asm__ volatile("dsb sy" ::: "memory");
            munmap(lcdc, 4096);
        }
        if (mfd >= 0) close(mfd);
        VPUFreeLinear(&tgt);
    }

    printf("bump: frames=%d dec_avg=%.1fms dec_min=%.1fms dec_max=%.1fms es=%ldB fb=%s",
           frames, frames ? dec_total / frames : 0.0,
           dec_min < 1e9 ? dec_min : 0.0, dec_max, es_size,
           no_fb ? "off" : "on");
    return frames > 0 ? 0 : 2;
}
