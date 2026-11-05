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
- Open bug: sometimes `decode_getframe` returns pool buffers with no pixel
  data at all (flat, unwritten; metadata fine, err=0) — seen both on a wedged
  session and on a fresh boot, while the same binary produced real pixels in
  other runs. Under investigation.

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

**Open:** the pattern renders with periodic red squares = VOP fetch faults.
The VOP has an IOMMU (registers at VOP+0x300…0x324, `DTE_ADDR=0x67153000`,
active at boot) that doesn't know about my buffer. Next step: map the buffer
into its page table or disable paging.

SurfaceFlinger must be stopped while writing VOP registers — with SF running,
register pokes watchdog-reboot the box ~10 minutes later; with SF stopped, no
reboot observed.

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

1. VOP IOMMU: map my pages or disable paging → kills the red squares.
2. The empty-frames decoder bug.
3. Live path: pre-bind the RTP port, receive a real cast, decode into win0.
4. 1080p via CEA masks in the WFD negotiation + a 256-byte EDID
   (`tools/edid_builder.py` builds the 128-byte variant).
