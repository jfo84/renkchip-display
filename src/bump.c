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
#include <sys/select.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <netinet/in.h>
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
static unsigned int phys_of(void *vaddr);

static unsigned char *uc_alias(void *vir, unsigned int len)
{
    /* Resolve via the fopen variant: the long-lived pm_fd read path
     * returned 0 for addresses this resolves fine (fd likely closed or
     * reused under us by the vpu shim). One fopen per new page is cheap
     * — aliases are cached below. phys_of_fd stays for A/B diagnostics. */
    unsigned int phy = phys_of(vir);
    if (!phy) {
        unsigned int pfd = phys_of_fd(pm_fd, vir);
        fprintf(stderr, "uc_alias: vir=%p fopen=%08x fd=%08x\n", vir, phy, pfd);
        return NULL;
    }
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


/* Resolve an allocation's TRUE physical address when pagemap lies
 * (Nov 6 boots: PFNs were garbage → writes went to phantom regions,
 * the VOP scanned zeros → the green screen). Plant a cookie through
 * the cached pointer (buffer > L2, so writes evict to DRAM), then
 * hunt /dev/mem for it: CMA first (kernel dmesg: 8 MiB at 0x67800000),
 * then all of DRAM. */
static unsigned int find_buf_phys(void *vaddr, unsigned int len)
{
    static const unsigned char cookie[16] = {
        0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xF0, 0x0F,
        0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xF0, 0x0F};
    unsigned char *p = (unsigned char *)vaddr;
    for (unsigned int i = 0; i + 16 <= len; i += 16)
        memcpy(p + i, cookie, 16);
    int mfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mfd < 0) return 0;
    /* CMA ONLY (kernel dmesg: 8 MiB @ 0x67800000). Scanning all of DRAM
     * maps reserved/hole regions and PANICS this kernel — killed the box
     * once. If the cookie isn't in CMA, fail cleanly instead. */
    const struct { unsigned int start, span; } zones[1] = {
        {0x67800000, 0x01000000},
    };
    for (int z = 0; z < 1; z++)
        for (unsigned int a = zones[z].start;
             a < zones[z].start + zones[z].span; a += 0x1000) {
            unsigned char *m = mmap(NULL, 0x1000, PROT_READ,
                                    MAP_SHARED, mfd, a);
            if (m == MAP_FAILED) continue;
            if (!memcmp(m, cookie, 16)) {
                munmap(m, 0x1000); close(mfd);
                return a;
            }
            munmap(m, 0x1000);
        }
    close(mfd);
    return 0;
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
    *sys_ctrl &= ~(1u << 1);                           /* WIN1_EN off — the
                                     * vendor player's overlay stays latched
                                     * after casts (green bars over our win0;
                                     * m_WIN1_EN = 1<<1 per vendor header) */
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

    if (strcmp(fb_dev, "memdump") == 0) {
        /* raw phys memory dumper: bump x memdump <hexphy> <len> */
        if (argc > 3) {
            unsigned int mp = (unsigned int)strtoul(argv[3], NULL, 16);
            int ml = argc > 4 ? atoi(argv[4]) : 4096;
            mp &= ~0xfffu;
            int mfd = open("/dev/mem", O_RDWR | O_SYNC);
            if (mfd < 0) { printf("memdump: open failed\n"); return 1; }
            unsigned char *m = mmap(NULL, ml, PROT_READ, MAP_SHARED, mfd, mp);
            if (m == MAP_FAILED) { printf("memdump: mmap %08x failed\n", mp); return 1; }
            FILE *mo = fopen("/mnt/ram0/mem.bin", "wb");
            if (mo) { fwrite(m, 1, ml, mo); fclose(mo); }
            munmap(m, ml); close(mfd);
            printf("memdump: %08x %d bytes ok\n", mp, ml);
        }
        return 0;
    }

    if (strcmp(fb_dev, "memscan") == 0) {
        /* bump x memscan <start_hex> <span> <step> <len>: classify DRAM sites */
        if (argc > 4) {
            unsigned int sa = (unsigned int)strtoul(argv[3], NULL, 16) & ~0xfffu;
            unsigned int span = (unsigned int)strtoul(argv[4], NULL, 16);
            unsigned int step = argc > 5 ? (unsigned int)strtoul(argv[5], NULL, 16) : 0x400000;
            int sl = argc > 6 ? atoi(argv[6]) : 4096;
            if (step < 0x1000) step = 0x1000;
            int mfd = open("/dev/mem", O_RDWR | O_SYNC);
            if (mfd < 0) { printf("memscan: open failed\n"); return 1; }
            for (unsigned int a = sa; a < sa + span; a += step) {
                unsigned char *m = mmap(NULL, sl, PROT_READ, MAP_SHARED, mfd, a);
                if (m == MAP_FAILED) { printf("%08x MAPFAIL\n", a); continue; }
                int distinct[256] = {0};
                for (int i = 0; i < sl; i++) distinct[m[i]]++;
                int nd = 0; int top = 0; int topn = 0;
                for (int b = 0; b < 256; b++) {
                    if (distinct[b]) nd++;
                    if (distinct[b] > topn) { topn = distinct[b]; top = b; }
                }
                printf("%08x d=%d top=%02x(%d/%d)\n", a, nd, top, topn, sl);
                munmap(m, sl);
            }
            close(mfd);
        }
        return 0;
    }

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

    if (strcmp(fb_dev, "mmu") == 0 || strcmp(fb_dev, "mmuoff") == 0 ||
        strcmp(fb_dev, "mmuon") == 0 || strcmp(fb_dev, "mmuclr") == 0) {
        /* VOP IOMMU: the red squares on our win0 pattern are periodic
         * fetch faults (MMU_INT_RAWSTAT page-fault latched, fault addr
         * cycles). Lever (header-documented, so first): clear m_MMU_EN
         * bit10 of AXI_BUS_CTRL(0x2C) — bypasses translation entirely.
         * MMU_COMMAND(0x308) paging-disable is the fallback lever; its
         * command encoding is NOT in the vendor header, so it is not
         * written blind. One lever per run for attribution. */
        int off = strcmp(fb_dev, "mmuoff") == 0;
        int on = strcmp(fb_dev, "mmuon") == 0;  /* vendor content needs the
                                                 * MMU back: SF buffers only
                                                 * render through translation */
        int clr = strcmp(fb_dev, "mmuclr") == 0; /* a latched PAGE_FAULT_ACTIVE
                                                  * (sta bit1) can wedge fetches
                                                  * even after m_MMU_EN is
                                                  * restored; clear both ints */
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        unsigned char *lcdc = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, mfd, 0x10118000);
        if (lcdc == MAP_FAILED) {
            printf("bump: lcdc mmap failed: %s", strerror(errno));
            return 1;
        }
        unsigned int *axi = (unsigned int *)(lcdc + 0x2C);
        unsigned int *dte = (unsigned int *)(lcdc + 0x300);
        unsigned int *sta = (unsigned int *)(lcdc + 0x304);
        unsigned int *cmd = (unsigned int *)(lcdc + 0x308);
        unsigned int *pfa = (unsigned int *)(lcdc + 0x30C);
        unsigned int *raw = (unsigned int *)(lcdc + 0x314);
        unsigned int *icr = (unsigned int *)(lcdc + 0x318);
        unsigned int *ims = (unsigned int *)(lcdc + 0x320);
        printf("mmu before: axi=%08x dte=%08x sta=%08x cmd=%08x\n",
               *axi, *dte, *sta, *cmd);
        printf("mmu before: pfaddr=%08x raw=%08x ints=%08x\n",
               *pfa, *raw, *ims);
        if (off) {
            *axi &= ~(1u << 10);              /* m_MMU_EN off = bypass */
            __asm__ volatile("dsb sy" ::: "memory");
        }
        if (on) {
            *axi |= (1u << 10);               /* m_MMU_EN on = translate */
            __asm__ volatile("dsb sy" ::: "memory");
        }
        if (clr) {
            *icr = 3;                         /* fault + bus-error clear */
            __asm__ volatile("dsb sy" ::: "memory");
        }
        printf("mmu after : axi=%08x dte=%08x sta=%08x cmd=%08x\n",
               *axi, *dte, *sta, *cmd);
        printf("mmu after : pfaddr=%08x raw=%08x ints=%08x\n",
               *pfa, *raw, *ims);
        munmap(lcdc, 4096);
        close(mfd);
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
        int got = 0, nf = 0;
        int nfwant = argc > 3 ? atoi(argv[3]) : 8;
        /* empty-frames lever: walk frames 1..N, sample each at three Y
         * points + UV start, cached AND uncached. A pixel-bearing frame
         * has spread across the points; a pool-recycled empty one is
         * flat. Free each frame after inspecting (main-loop pattern). */
        while (poff < psz && nf < nfwant) {
            long au = next_au(pes, psz, poff + 4);
            long chunk = (au > 0 ? au : psz) - poff;
            VideoPacket_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.data = pes + poff;
            pkt.size = chunk;
            pctx->decode_sendstream(pctx, &pkt);
            if (pkt.size == 0) poff += chunk; else usleep(2000);
            for (int r = 0; r < 50 && nf < nfwant; r++) {
                memset(&pv, 0, sizeof(pv));
                memset(&pout, 0, sizeof(pout));
                pout.data = (unsigned char *)&pv;
                pctx->decode_getframe(pctx, &pout);
                if (!(pout.size && pv.vpumem.vir_addr)) { usleep(2000); continue; }
                got = 1;
                nf++;
                unsigned char *v = (unsigned char *)pv.vpumem.vir_addr;
                unsigned int w = pv.FrameWidth, h = pv.FrameHeight;
                unsigned int ysz = w * h;
                unsigned int pts[4] = { 0, ysz / 2, ysz - 16, ysz };
                unsigned char *ucp[4];
                fprintf(stderr, "probe: f%d/%d vir=%p fw=%u fh=%u err=%#x phy=%08x fd=%08x\n",
                        nf, nfwant, v, w, h, pv.ErrorInfo,
                        phys_of(v), phys_of_fd(pm_fd, v));
                for (int k = 0; k < 4; k++) {
                    ucp[k] = uc_alias(v + pts[k], 16);
                    fprintf(stderr, "probe:  p%d off=%u c=%02x%02x u=%02x%02x\n",
                            k, pts[k], v[pts[k]], v[pts[k] + 1],
                            ucp[k] ? ucp[k][0] : 0xee, ucp[k] ? ucp[k][1] : 0xee);
                }
                if (nf == 1 || nf == nfwant) {
                    char fn[64];
                    snprintf(fn, sizeof fn, "/mnt/ram0/fr%d_p0.bin", nf);
                    FILE *d = fopen(fn, "wb");
                    if (d && ucp[0]) { fwrite(ucp[0], 1, 0x1000, d); fclose(d); }
                    else if (d) fclose(d);
                }
                VPUFreeLinear(&pv.vpumem);
            }
        }
        if (!got) {
            printf("bump: probe got no frame");
            return 1;
        }
        pctx->flush(pctx);
        vpu_close_context(&pctx);
        fprintf(stderr, "probe: done %d frames\n", nf);
        return 0;
    }

    if (strcmp(fb_dev, "live") == 0) {
        /* M2: tap the cast's UDP 15550 with AF_PACKET while it runs (a
         * bound :15550 breaks the sink's session setup — tap, don't bind).
         * RTP PT33 → MPEG2-TS → PAT/PMT → PES (video PES_length is 0,
         * so AUs run PUSI→PUSI) → H.264 ES → next_au split → decoder →
         * win0 scanout target. MMU is bypassed while we own the display
         * and restored on exit (vendor content needs translation). */
        VpuCodecContext_t *ctx = NULL;
        if (vpu_open_context(&ctx) != 0 || !ctx) {
            printf("bump: live open_ctx failed");
            return 1;
        }
        ctx->codecType = CODEC_DECODER;
        ctx->videoCoding = OMX_RK_VIDEO_CodingAVC;
        ctx->width = 1280;
        ctx->height = 720;
        ctx->enableparsing = 1;
        ctx->no_thread = 1;
        if (ctx->init(ctx, NULL, 0) != 0) {
            printf("bump: live init failed");
            return 1;
        }
        static VPUMemLinear_t tgt;
        if (VPUMallocLinear(&tgt, 1280 * 720 * 3 / 2) != 0 || !tgt.vir_addr) {
            printf("bump: live VPUMallocLinear failed");
            return 1;
        }
        unsigned int tgt_phy = find_buf_phys(tgt.vir_addr, 1280 * 720 * 3 / 2);
        if (!tgt_phy) {
            printf("bump: live cookie-resolve failed");
            return 1;
        }
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mfd < 0) {
            printf("bump: /dev/mem open failed: %s", strerror(errno));
            return 1;
        }
        unsigned char *tm = mmap(NULL, 1280 * 720 * 3 / 2,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 mfd, tgt_phy);
        unsigned char *mmu = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, mfd, 0x10118000);
        if (tm == MAP_FAILED || mmu == MAP_FAILED) {
            printf("bump: live mmap failed: %s", strerror(errno));
            return 1;
        }
        memset(tm, 0x10, 1280 * 720 * 3 / 2);
        unsigned int *axi = (unsigned int *)(mmu + 0x2C);
        unsigned int axi_was = *axi;
        int took_display = 0;

        /* TAP, not bind: a bound :15550 makes the sink's session setup
         * FAIL (player can't bind → wfd aborts — proven twice tonight).
         * AF_PACKET clone: the vendor player keeps its port and the
         * session stays normal; we mirror the RTP from the wire. */
        int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
        if (s < 0) {
            printf("bump: AF_PACKET socket failed: %s", strerror(errno));
            return 1;
        }
        int rb = 4 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);  /* best effort */
        fprintf(stderr, "live: tapping :15550 (AF_PACKET) tgt=%08x — cast away\n",
                tgt_phy);

        unsigned char pkt[2048];
        unsigned char pes[256 * 1024];          /* PES under construction */
        unsigned int peslen = 0, pescap = sizeof pes;
        static unsigned char es[4 * 1024 * 1024]; /* unsplit AU window */
        unsigned int esr = 0, esw = 0;
        int vpid = -1, pmt_pid = -1, in_pes = 0;
        unsigned rtp_last = 0, rtp_lost = 0, rtp_cnt = 0;
        long pes_cnt = 0, frames = 0;
        int win0_done = 0;
        double t_last = now_ms();
        double t_feed = now_ms(), t_stat = now_ms();
        unsigned last_fw = 0, last_fh = 0;
        int last_sr = 0;
        VPU_FRAME vframe;
        DecoderOut_t out;
        VideoPacket_t vp;

        while (1) {
            double idle = now_ms() - t_last;
            /* 30-min no-cast patience: the human discovery+connect dance
             * (Win+K refresh, slow listing) took >5 min once and cost us
             * the session at exactly 300s. */
            if (idle > (frames ? 10000 : 1800000)) break;
            struct timeval tv = { 0, 20000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            if (select(s + 1, &fds, NULL, NULL, &tv) <= 0) {
                /* sparse-stream flush: a static screen may never produce
                 * the NEXT slice start code, so next_au would never cut
                 * an AU. Time-flush the pending window to the decoder. */
                if (esw - esr > 8 && now_ms() - t_feed > 500) {
                    memset(&vp, 0, sizeof vp);
                    vp.data = es + esr;
                    vp.size = esw - esr;
                    esr = esw;
                    last_sr = ctx->decode_sendstream(ctx, &vp);
                    t_feed = now_ms();
                    for (int k = 0; k < 4; k++) {
                        memset(&vframe, 0, sizeof vframe);
                        memset(&out, 0, sizeof out);
                        out.data = (unsigned char *)&vframe;
                        ctx->decode_getframe(ctx, &out);
                        if (out.size == 0 || !vframe.vpumem.vir_addr)
                            break;
                        last_fw = vframe.FrameWidth;
                        last_fh = vframe.FrameHeight;
                        if (last_fw && last_fh && last_fw <= 1280 &&
                            last_fh <= 720) {
                            if (!win0_done) {
                                win0_program(tgt_phy - 0x50000000,
                                             tgt_phy - 0x50000000 +
                                                 last_fw * last_fh,
                                             last_fw);
                                win0_done = 1;
                                fprintf(stderr,
                                        "live: win0 y=%08x fw=%u fh=%u\n",
                                        tgt_phy - 0x50000000, last_fw,
                                        last_fh);
                            }
                            memcpy(tm, vframe.vpumem.vir_addr,
                                   last_fw * last_fh * 3 / 2);
                        }
                        VPUFreeLinear(&vframe.vpumem);
                        frames++;
                    }
                }
                /* 1Hz vitals: what the tap, PES path and decoder did */
                if (now_ms() - t_stat > 1000) {
                    fprintf(stderr,
                            "live: rtp=%u lost=%u pes=%ld pend=%u f=%ld "
                            "sr=%d fw=%u fh=%u\n",
                            rtp_cnt, rtp_lost, pes_cnt, esw - esr, frames,
                            last_sr, last_fw, last_fh);
                    t_stat = now_ms();
                }
                continue;
            }
            int n = recvfrom(s, pkt, sizeof pkt, 0, NULL, NULL);
            /* eth → IP → UDP:15550 → RTP. Fall back to a 0x45 scan if
             * the link layer isn't plain Ethernet II on this radio. */
            unsigned char *ip = NULL;
            if (n > 34 && pkt[12] == 0x08 && pkt[13] == 0x00) ip = pkt + 14;
            else {
                for (int i = 14; i < 34 && i + 20 <= n; i++)
                    if (pkt[i] == 0x45) { ip = pkt + i; break; }
            }
            if (!ip || (ip[0] >> 4) != 4 || ip[9] != 17) continue;
            int ihl = (ip[0] & 0xF) * 4;
            unsigned char *udp = ip + ihl;
            int dport = (udp[2] << 8) | udp[3];
            if (dport != 15550) continue;
            unsigned char *r = udp + 8;
            int rn = n - (r - pkt);
            if (rn < 12 || (r[0] >> 6) != 2) continue;
            unsigned char *pkt_rtp = r;
            t_last = now_ms();
            if (!took_display) {
                /* First RTP: the session is live (discovery+connect+PLAY
                 * happened on the pristine vendor stack). NOW take the
                 * display: stop SF so it stops committing over our win0,
                 * and bypass the VOP MMU for our untranslated fetches. */
                system("/system/bin/setprop ctl.stop surfaceflinger");
                *axi &= ~(1u << 10);
                __asm__ volatile("dsb sy" ::: "memory");
                took_display = 1;
                fprintf(stderr, "live: first RTP — took display\n");
            }
            int hl = 12 + 4 * (pkt_rtp[0] & 0xF);
            if (pkt_rtp[0] & 0x10) {            /* RTP extension */
                if (hl + 4 <= rn)
                    hl += 4 + 4 * ((pkt_rtp[hl + 2] << 8) | pkt_rtp[hl + 3]);
            }
            if (hl >= rn) continue;
            unsigned seq = (pkt_rtp[2] << 8) | pkt_rtp[3];
            if (rtp_last) {
                unsigned d = (seq - rtp_last - 1) & 0xFFFF;
                if (d < 0x8000) rtp_lost += d;
            }
            rtp_last = seq;
            rtp_cnt++;
            unsigned char *p = pkt_rtp + hl;
            int plen = rn - hl;

            for (int o = 0; o + 188 <= plen; o += 188) {
                unsigned char *t = p + o;
                if (t[0] != 0x47) continue;      /* no resync: lose the pkt */
                int pid = ((t[1] & 0x1F) << 8) | t[2];
                int pusi = t[1] & 0x40;
                int afc = (t[3] >> 4) & 3;
                int q = 4;
                if (afc & 2) q += 1 + t[4];
                if (!(afc & 1) || q >= 188) continue;
                unsigned char *pl = t + q;
                int pll = 188 - q;

                if (pid == 0 && pusi && pmt_pid < 0) {
                    unsigned char *sec = pl + 1 + pl[0];
                    if (pl[0] <= 32 && pll >= (int)pl[0] + 14)
                        pmt_pid = ((sec[10] << 8) | sec[11]) & 0x1FFF;
                    fprintf(stderr, "live: PAT pmt_pid=%d\n", pmt_pid);
                    continue;
                }
                if (pmt_pid > 0 && pid == pmt_pid && pusi && vpid < 0) {
                    unsigned char *sec = pl + 1 + pl[0];
                    if (pll <= 16) continue;
                    int slen = ((sec[1] & 0xF) << 8) | sec[2];
                    int pilen = ((sec[10] & 0xF) << 8) | sec[11];
                    int pp = 12 + pilen, apid = -1;
                    int lim = 3 + slen - 4;
                    if (lim > pll) lim = pll;
                    while (pp + 5 <= lim) {
                        int st = sec[pp];
                        int ep = ((sec[pp + 1] & 0x1F) << 8) | sec[pp + 2];
                        int el = ((sec[pp + 3] & 0xF) << 8) | sec[pp + 4];
                        if (st == 0x1B && vpid < 0) vpid = ep;
                        if (st == 0x0F) apid = ep;
                        pp += 5 + el;
                    }
                    fprintf(stderr, "live: PMT vpid=%d apid=%d\n", vpid, apid);
                    continue;
                }
                if (pid != vpid) continue;       /* video only for now */

                if (pusi && in_pes) {            /* close PES → ES tail */
                    if (peslen > 9 && pes[0] == 0 && pes[1] == 0 &&
                        pes[2] == 1) {
                        unsigned int off = 9 + pes[8];
                        if (off < peslen) {
                            if (esw + (peslen - off) > sizeof es) {
                                memmove(es, es + esr, esw - esr);
                                esw -= esr;
                                esr = 0;
                            }
                            if (esw + (peslen - off) <= sizeof es) {
                                memcpy(es + esw, pes + off, peslen - off);
                                esw += peslen - off;
                                pes_cnt++;
                            }
                        }
                    }
                    in_pes = 0;
                    peslen = 0;
                    /* feed every complete AU in the window */
                    long au;
                    while (esw - esr > 8 &&
                           (au = next_au(es, esw, esr + 4)) > 0) {
                        memset(&vp, 0, sizeof vp);
                        vp.data = es + esr;
                        vp.size = au - esr;
                        esr = au;
                        last_sr = ctx->decode_sendstream(ctx, &vp);
                        t_feed = now_ms();
                        for (int k = 0; k < 4; k++) {   /* drain */
                            memset(&vframe, 0, sizeof vframe);
                            memset(&out, 0, sizeof out);
                            out.data = (unsigned char *)&vframe;
                            ctx->decode_getframe(ctx, &out);
                            if (out.size == 0 || !vframe.vpumem.vir_addr)
                                break;
                            unsigned int hs = vframe.FrameWidth;
                            unsigned int vs = vframe.FrameHeight;
                            last_fw = hs; last_fh = vs;
                            if (hs && vs && hs <= 1280 && vs <= 720) {
                                if (!win0_done) {
                                    win0_program(tgt_phy - 0x50000000,
                                                 tgt_phy - 0x50000000 +
                                                     hs * vs, hs);
                                    win0_done = 1;
                                    fprintf(stderr,
                                            "live: win0 y=%08x fw=%u fh=%u\n",
                                            tgt_phy - 0x50000000, hs, vs);
                                }
                                memcpy(tm, vframe.vpumem.vir_addr,
                                       hs * vs * 3 / 2);
                            }
                            VPUFreeLinear(&vframe.vpumem);
                            frames++;
                            if ((frames & 31) == 0)
                                fprintf(stderr,
                                        "live: f%ld rtp=%u lost=%u pes=%ld\n",
                                        frames, rtp_cnt, rtp_lost, pes_cnt);
                        }
                    }
                }
                if (pusi) {
                    in_pes = 1;
                    peslen = 0;
                }
                if (in_pes && peslen + pll <= pescap) {
                    memcpy(pes + peslen, pl, pll);
                    peslen += pll;
                } else if (in_pes) {
                    peslen = 0;                  /* oversize: drop PES */
                    in_pes = 0;
                }
            }
        }
        fprintf(stderr, "live: end f%ld rtp=%u lost=%u pes=%ld idle-break\n",
                frames, rtp_cnt, rtp_lost, pes_cnt);
        close(s);
        if (win0_done) {
            /* hand the display back: boot win0 + MMU translation */
            unsigned int *sys_ctrl = (unsigned int *)(mmu + 0x00);
            unsigned int *yrgb_mst = (unsigned int *)(mmu + 0x20);
            unsigned int *cbr_mst = (unsigned int *)(mmu + 0x24);
            unsigned int *cfg_done = (unsigned int *)(mmu + 0x90);
            *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (1u << 15);
            *yrgb_mst = 0x10b88000;
            *cbr_mst = 0;
            *axi = axi_was;
            __asm__ volatile("dsb sy" ::: "memory");
            *cfg_done = 0x01;
            __asm__ volatile("dsb sy" ::: "memory");
        }
        munmap(tm, 1280 * 720 * 3 / 2);
        munmap(mmu, 4096);
        close(mfd);
        /* tgt is fb0's scanout region — never free */
        ctx->flush(ctx);
        vpu_close_context(&ctx);
        printf("bump: live done frames=%ld rtp=%u lost=%u", frames,
               rtp_cnt, rtp_lost);
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
    /* hold: like pattern (no exit restore — win0 keeps scanning our tgt),
     * but WITHOUT the white/black test-pattern overwrite: the last decoded
     * frame stays on screen. For the ops-page still-image pushes. */
    int hold_mode = phys_mode && argc > 3 && strcmp(argv[3], "hold") == 0;
    if (phys_mode) {
        if (VPUMallocLinear(&tgt, 1280 * 720 * 3 / 2) != 0 || !tgt.vir_addr) {
            printf("bump: VPUMallocLinear failed");
            return 1;
        }
        tgt_phy = find_buf_phys(tgt.vir_addr, 1280 * 720 * 3 / 2);
        if (!tgt_phy) {
            printf("bump: cookie-resolve failed");
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
                           vframe.FrameWidth * vframe.FrameHeight * 3 / 2, df); \
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
                /* CACHED vir_addr is the only reliable view of the real
                 * decode pool (pagemap/uc_alias maps garbage for it — the
                 * green-bars bug; live mode proved cached reads correct
                 * for 300+ frames on Nov 6). Write side stays the uncached
                 * tgt alias `tm`. */                                    \
                const unsigned char *src = (unsigned char *)vframe.vpumem.vir_addr; \
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
                if (hold_mode) {                                            \
                    /* skip decoder placeholder frames (flat Y=0x10 warmup \
                     * buffers — the green-bars bug: wfdcap8 starts mid-GOP \
                     * so its first outputs are flat). Hold the first frame \
                     * with real variance. */                               \
                    int flat = 1;                                            \
                    for (long q = 0; q < (long)hs * vs; q += 3943)          \
                        if (src[q] != src[0]) { flat = 0; break; }           \
                    if (flat) {                                              \
                        VPUFreeLinear(&vframe.vpumem);                      \
                        continue;   /* placeholder: keep draining */        \
                    }                                                        \
                    fprintf(stderr, "stage:hold real frame #%ld\n", frames);\
                    eos = 1;   /* accept: memcpy below runs, then stop */   \
                }                                                            \
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
        /* hold WITHOUT restoring/freeing: win0 keeps scanning our intact
         * buffer, so the pattern stays up for visual judgment. (The old
         * 90s + restore path left YRGB_MST pointing at the FREED buffer
         * as ARGB888 — that was the purple/indigo strikes, not the MMU.) */
        fprintf(stderr, "stage:pattern holding 600s\n");
        sleep(600);
        return 0;
    }
    if (hold_mode) {
        /* still-image hold: like pattern but the decoded frame stays;
         * the ops-page loop restarts us with a fresh frame on change */
        fprintf(stderr, "stage:hold holding\n");
        while (1) pause();
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
            unsigned int *yrgb_mst = (unsigned int *)(lcdc + 0x20);
            unsigned int *cbr_mst = (unsigned int *)(lcdc + 0x24);
            unsigned int *cfg_done = (unsigned int *)(lcdc + 0x90);
            *sys_ctrl = (*sys_ctrl & ~(7u << 3)) | (1u << 15);
            *yrgb_mst = 0x10b88000;   /* boot MST — was left dangling at ours */
            *cbr_mst = 0;
            __asm__ volatile("dsb sy" ::: "memory");
            *cfg_done = 0x01;
            __asm__ volatile("dsb sy" ::: "memory");
            munmap(lcdc, 4096);
        }
        if (mfd >= 0) close(mfd);
        /* tgt is fb0's scanout region — never free */
    }

    printf("bump: frames=%d dec_avg=%.1fms dec_min=%.1fms dec_max=%.1fms es=%ldB fb=%s",
           frames, frames ? dec_total / frames : 0.0,
           dec_min < 1e9 ? dec_min : 0.0, dec_max, es_size,
           no_fb ? "off" : "on");
    return frames > 0 ? 0 : 2;
}
