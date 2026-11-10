# renkchip-display

**Turning a cheap Renkchip Q2 Miracast stick into a tiny display node.**

The goal: an ordinary TV hanging on the wall showing a quiet status page,
driven by a $20 stick. As of Nov 2026 the TV shows the page — through the
stick's stock DLNA renderer, in a self-healing loop — and the "no vendor
stack in the way" version is proven too: a live cast tapped off the air,
decoded on the VPU, scanned out by the display controller directly.

## What this is

Original code and research findings from liberating a **Renkchip Q2 Miracast
HDMI stick** (Rockchip RK3036, Android 5.1) that I own. Two tracks:

- **The practical display node** — render the page on a real computer,
  serve it from the LAN, and let the stick's stock DLNA player show it at
  1080p, forever. `scripts/ops_dlna.sh` + the desktop server; the
  constraints live in `docs/DLNA-STATUS-PIPELINE.md`.
- **The own receive path** — no vendor player in the loop: tap a live
  cast's RTP with AF_PACKET, demux to H.264, decode through the VPU,
  program win0 to scan my buffer out directly. `src/bump.c`; the
  reverse-engineering story is `docs/FINDINGS.md`.

All research was done on hardware I own, on my own network.

## Status (Nov 2026)

- **Status board running on the TV at 1080p** (Nov 10): the page renders
  headless, is served from a wired desktop, and the stick's stock DLNA
  renderer plays it in a self-healing loop — 9-hour soak, zero flaps. No
  root, no vendor-stack surgery. See `docs/DLNA-STATUS-PIPELINE.md`.
- Miracast / Wi-Fi Display session reverse-engineered from captures of my own
  traffic; H.264 elementary stream extracted and replayed.
- Hardware decode proven: 128/128 frames through `libvpu` (legacy API).
- Direct display-controller programming proven: win0 scanout of my own buffer
  renders a YUV test pattern on the panel, hardware-scaled 720p → 1080p.
  VOP IOMMU fetch faults (red squares) fixed by MMU bypass (`mmuoff`).
- **Full live path proven (Nov 6)**: a real cast's RTP tapped off the air
  (AF_PACKET), demuxed to H.264, decoded, scanned out on win0 — 300+ frames
  of live 720p through the whole own pipeline (`bump x live`).
- Open: 1080p on the own path via EDID/CEA negotiation; the boot-to-boot
  pagemap garbage behind the green screen (see `docs/FINDINGS.md`).

## Layout

| Path | What |
| --- | --- |
| `src/bump.c` | On-device lab program: live cast tap → H.264 decode → VPU buffer → win0 scanout, plus VOP MMU and memory-forensics modes |
| `src/demux.c`, `src/demux.h` | The cast-tap parse path (RTP → TS → PES → H.264 AUs), extracted so it builds and tests on a host |
| `tests/test_demux.c` | Demuxer tests: synthetic TS/PES/RTP vectors built in C, no captured data |
| `scripts/` | Host-side C2: arm, re-arm after reboot, remote shell, build |
| `scripts/ops_dlna.sh` | The TV status driver: render → serve → SOAP-play loop |
| `scripts/serve_ops.ps1`, `scripts/setup_ops.ps1` | Desktop file server + one-shot setup |
| `tools/` | PCAP → H.264 ES extraction, EDID builder, log puller |
| `docs/FINDINGS.md` | The technical write-up: boot state, traps, register recipes |
| `docs/DLNA-STATUS-PIPELINE.md` | The working TV pipeline and its hard-won constraints |

## Building

`bump.c` cross-compiles with an Android NDK (armv7a, API 21) and links against
the vendor's legacy VPU API. Those headers and `libvpu.so` are **not in this
repo** — pull them from your own device or the vendor SDK, drop them next to
`bump.c`, then:

    NDK_CC=path/to/armv7a-linux-androideabi21-clang ./scripts/build_bump.sh

The demuxer tests need nothing but a C compiler — no vendor headers, no
hardware. They run on any host:

    ./scripts/run_tests.sh

The scripts reference my lab IPs (stick at `192.168.8.160`, desktop at
`192.168.8.219`); adjust to your own. `scripts/server2.py` serves the script
directory on :8000 and queues commands; `scripts/cycle.sh` is the one-shot
"the stick rebooted, bring everything back" entry point.

## Scope and legal

- Everything in this repo is my own code or my own observations. It
  deliberately contains **no vendor material**: no firmware images, no memory
  dumps, no vendor libraries or binaries. Those are copyrighted by their
  makers; I can't redistribute them, so I don't. Where my tooling needs a
  vendor binary (e.g. the modified Wi-Fi Display daemon the scripts deploy),
  the repo references it by name only — bring your own.
- My code is MIT-licensed (see `LICENSE`), provided as-is, no warranty.
- Not affiliated with, endorsed by, or supported by Renkchip or Rockchip.
  All names are used for identification only.
