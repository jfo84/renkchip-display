# FINDINGS — Renkchip Q2 (RK3036) display research

Working notes from liberating this stick. All observations are my own, taken
from my own device and my own network traffic. Register offsets are documented
inline in `src/bump.c` and in the recipes below.

## The device

- Renkchip Q2 Miracast stick: Rockchip RK3036, Android 5.1.1, 16 MB SPI flash,
  RTL8811CU wifi, Boa 0.94.13 web admin UI.
- Framebuffers: fb0 1280x1440, fb2 1280x720 — but SurfaceFlinger owns the
  display; raw framebuffer writes go nowhere visible. The real lever is the
  display controller (VOP/LCDC) at `0x10118000`.

## Getting a shell

The stock web UI runs as root, and `connectSavedAp.cgi` passes the SSID string
through a shell. So a "network name" like `A$(command)` executes on the stick.
One command per injection (`&&` is eaten by the CGI parser; `;` chains run only
the first command; the CGI shell has an empty PATH — use absolute busybox
paths). Works instantly in station mode; in hotspot mode there is an async
window via POST to `add_network_connect.cgi`.

The working setup: `scripts/rearm.sh` injects a poller (`t4.sh`), which
rebuilds busybox applet symlinks and bind-mounts my replacement wfd daemon
(vendor-derived, not published). `scripts/rsh.sh` queues commands through
`scripts/server2.py`; output comes back as files under the web docroot.

## Wi-Fi Display / RTSP

- P2P on channel 36 (`p2p_oper_channel=36`, `SM_MODE_STA_P2P`).
- Session shape: RTSP negotiate → PLAY → RTP / MPEG-TS / H.264 720p.
- `tools/extract_es.py` recovers the H.264 (+ AAC) elementary streams from a
  pcapng of my own cast traffic (RTP PT 33 → TS → PES → ES; pktmon's
  etl2pcap duplicates every packet, dedup by seq+payload).

## Hardware decode

- `libvpu` on this firmware is a shim over libmpp. The legacy `VpuApiLegacy`
  API works: init, feed H.264 ES, `decode_getframe` → NV12. 128/128 frames
  decoded from a captured cast.
- The shim lies: `VPUMemClean/Flush/Invalidate/Link` are 8-byte RET stubs —
  all cache operations are no-ops. The `phy_addr` / `FrameBusAddr` fields in
  the legacy structs are garbage (tiny fd-like numbers). `vir_addr` is the
  only truth; real physical addresses come from `/proc/self/pagemap`.
- **Pagemap can lie too** (Nov 6): on some boots the PFNs come back garbage
  — writes land in phantom regions and the panel shows the green screen (the
  VOP scanning zeros). Re-resolve with a fresh `fopen` of pagemap per page
  (the long-lived-fd read path goes bad under the shim); when that fails
  too, plant a 16-byte cookie through the cached pointer (buffer > L2, so it
  lands in DRAM) and hunt it in `/dev/mem` — **CMA only** (8 MiB at
  `0x67800000` on my unit). Scanning all of DRAM maps reserved/hole regions
  and panics this kernel. That resolver is `find_buf_phys()` in bump.c.
- The "empty frames" mystery, closed: `decode_getframe` returns flat,
  unwritten pool buffers until the first reference-complete output (~frame
  16–29, mid-GOP — a joiner can't render until its references exist).
  IDR-led live casts never show it. Warm-up behavior, not a bug.

## Display — the win0 path

Boot VOP state is deterministic every boot: `SYS_CTRL(0x00)=0x8081` (win0
enabled, ARGB888, RB swap), `YRGB_MST(0x20)=0x10b88000`, ACT 1280x720 →
DSP 1920x1080 (hardware scaler).

**The trap:** post-reboot, DRAM at `0x60b88000` belongs to the RAM disk. If
you find a scanout address during a vendor cast and later write it on a fresh
boot, you shred files on `/mnt/ram0` (0xFF runs inside binaries — confirmed)
and eventually watchdog-reboot the box.

**The correct recipe:**

1. Allocate your own buffer with `VPUMallocLinear`.
2. Resolve its physical address via `/proc/self/pagemap`.
3. Program win0 with bus address = phys − `0x50000000` (offset confirmed
   twice).
4. Write pixels through an **uncached** `/dev/mem` alias of the buffer, never
   the cached mapping.

win0 YUV420 programming: format bits[5:3] of `SYS_CTRL` = 4, `YRGB_MST` = Y
plane, `CBR_MST` = UV plane, BT.601 CSC (clear bits[21:20] of `DSP_CTRL0`),
then `cfg_done(0x90)=1` with a `dsb` before **and** after (register mappings
are Normal-NC; a bare latch write can overtake the config writes). Verified on
the panel: Y=0xFF / UV=0x00 renders as a saturated green test pattern.

The register-file readback lies — the driver repaints it with boot values.
Trust the panel, not the readback.

After a vendor cast, win1 can stay latched — green bars over our win0. Clear
WIN1_EN (bit 1 of `SYS_CTRL`) when taking the display; the code does.

**Red squares, resolved (Nov 6):** they were VOP IOMMU fetch faults. The VOP
has an MMU (registers at VOP+0x300…0x324, `DTE_ADDR=0x67153000`, active at
boot) that doesn't know about my buffer. Clearing **m_MMU_EN (bit 10 of the
AXI register, VOP+0x2C)** puts the VOP into bypass — clean fetches, squares
gone. Caveat: SurfaceFlinger's buffers only make sense translated, so hand
translation back (`mmuon`) before vendor content runs. `mmuoff` / `mmuon` /
`mmuclr` are first-class modes now.

Watchdog: VOP register writes trip the ~10-minute watchdog **even with
SurfaceFlinger stopped** (the earlier "no reboot with SF stopped" didn't
survive longer sessions). Plan raw-display sessions short.

## Box quirks

- busybox has no `dd`, `nohup`, `head`, `tail`, `md5sum`, or `curl` applets;
  `ps` segfaults intermittently; there is no `reboot` applet (power-cycle by
  hand).
- Long single-line command output dies in the word-by-word exfil channel —
  write to `/mnt/ram0`, symlink into Boa's docroot (symlinks survive reboots),
  and fetch over HTTP from the host.
- `/data` is a 768 KB, sick jffs2: new directories fail silently; do bulk work
  on `/mnt/ram0` (48 MB RAM disk).
- Vendor init sets every daemon `oom_score_adj=-1000` (unkillable), so the
  first OOM panics the kernel. `kharness.sh` makes daemons killable again.

## Where this goes next

1. ~~VOP IOMMU~~ — done via bypass (`mmuoff`); page-table mapping is the
   nicer long-term answer if vendor content must coexist with ours.
2. ~~Empty-frames decoder bug~~ — understood; decoder warm-up, see above.
3. ~~Live path~~ — done: `bump x live` taps the cast's RTP with AF_PACKET
   (a bound port breaks the sink's session setup — tap, don't bind),
   demuxes TS→PES→H.264, decodes, scans out on win0 with the MMU bypassed.
   300+ frames of a live 720p cast through the full own pipeline. The stall
   that hid it: a static screen may never produce an access unit, so pending
   input must be time-flushed to the decoder.
4. 1080p via CEA masks in the WFD negotiation + a 256-byte EDID
   (`tools/edid_builder.py` builds the 128-byte variant).
5. Boot-to-boot pagemap garbage — why PFNs come back bogus on some boots
   (the green screen). `find_buf_phys` works around it; the cause is open.
