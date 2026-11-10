/* demux.h — RTP/TS/PES/H.264 demux for the cast tap, extracted from
 * bump.c so the parsing path compiles and tests on a host. Pure C, no
 * device headers: feed link-layer frames, get complete access units
 * back through a callback. */
#ifndef DEMUX_H
#define DEMUX_H

#define DEMUX_PES_CAP (256 * 1024)      /* PES under construction */
#define DEMUX_ES_CAP  (4 * 1024 * 1024) /* unsplit AU window */

/* Called once per complete H.264 access unit. `au` points into the
 * demux ES window and is only valid until the call returns. */
typedef void (*au_cb)(void *ctx, const unsigned char *au, long len);

struct demux {
    /* found on the wire (PAT → PMT → video pid); -1 until seen */
    int pmt_pid, vpid, apid;
    /* PES under construction */
    unsigned char pes[DEMUX_PES_CAP];
    unsigned int peslen;
    int in_pes;
    /* unsplit ES window; compacted toward [0] when full */
    unsigned char es[DEMUX_ES_CAP];
    unsigned int esr, esw;
    /* vitals for the 1 Hz status line */
    unsigned rtp_cnt, rtp_lost, rtp_last;
    long pes_cnt, au_cnt;
};

/* Feed one link-layer frame (AF_PACKET style, Ethernet II or raw IP
 * with the 0x45 fallback scan). Returns 1 if the frame carried RTP on
 * the watched UDP port and was consumed, 0 if skipped. Calls cb for
 * every complete access unit found while consuming it. */
int demux_frame(struct demux *d, const unsigned char *frame, int n,
                unsigned short port, au_cb cb, void *ctx);

/* Flush the pending ES-window tail as one final access unit (sparse
 * streams: a static screen may never produce the next start code).
 * Returns flushed bytes (0 if the window was empty). */
long demux_flush(struct demux *d, au_cb cb, void *ctx);

/* Offset of the next picture start code (NAL type 1 or 5) at/after
 * `from`, -1 if none. Shared with bump's file-decode mode. */
long demux_next_au(const unsigned char *p, long size, long from);

#endif
