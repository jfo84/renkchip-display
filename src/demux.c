/* demux.c — the cast-tap parse path from bump.c, verbatim in logic:
 * eth → IP → UDP → RTP → MPEG2-TS → PAT/PMT → PES → H.264 ES → next_au.
 * Everything here is host-safe; the decoder and scanout policy stay in
 * bump.c behind the au callback. */
#include <stdio.h>
#include <string.h>
#include "demux.h"

long demux_next_au(const unsigned char *p, long size, long from)
{
    for (long i = from; i + 5 < size; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 &&
            ((p[i + 3] & 0x1F) == 1 || (p[i + 3] & 0x1F) == 5))
            return i;
    }
    return -1;
}

/* Close the PES under construction into the ES window, then feed every
 * complete AU in the window (the window holds the tail of the previous
 * AU until its successor's start code arrives). */
static void pes_close(struct demux *d, au_cb cb, void *ctx)
{
    if (d->peslen > 9 && d->pes[0] == 0 && d->pes[1] == 0 && d->pes[2] == 1) {
        unsigned int off = 9 + d->pes[8];
        if (off < d->peslen) {
            if (d->esw + (d->peslen - off) > DEMUX_ES_CAP) {
                memmove(d->es, d->es + d->esr, d->esw - d->esr);
                d->esw -= d->esr;
                d->esr = 0;
            }
            if (d->esw + (d->peslen - off) <= DEMUX_ES_CAP) {
                memcpy(d->es + d->esw, d->pes + off, d->peslen - off);
                d->esw += d->peslen - off;
                d->pes_cnt++;
            }
        }
    }
    d->in_pes = 0;
    d->peslen = 0;
    /* feed every complete AU in the window */
    long au;
    while (d->esw - d->esr > 8 &&
           (au = demux_next_au(d->es, d->esw, d->esr + 4)) > 0) {
        cb(ctx, d->es + d->esr, au - d->esr);
        d->esr = au;
        d->au_cnt++;
    }
}

static void ts_packet(struct demux *d, const unsigned char *t, au_cb cb,
                      void *ctx)
{
    if (t[0] != 0x47) return;              /* no resync: lose the pkt */
    int pid = ((t[1] & 0x1F) << 8) | t[2];
    int pusi = t[1] & 0x40;
    int afc = (t[3] >> 4) & 3;
    int q = 4;
    if (afc & 2) q += 1 + t[4];
    if (!(afc & 1) || q >= 188) return;
    const unsigned char *pl = t + q;
    int pll = 188 - q;

    if (pid == 0 && pusi && d->pmt_pid < 0) {
        const unsigned char *sec = pl + 1 + pl[0];
        if (pl[0] <= 32 && pll >= (int)pl[0] + 14)
            d->pmt_pid = ((sec[10] << 8) | sec[11]) & 0x1FFF;
        fprintf(stderr, "demux: PAT pmt_pid=%d\n", d->pmt_pid);
        return;
    }
    if (d->pmt_pid > 0 && pid == d->pmt_pid && pusi && d->vpid < 0) {
        const unsigned char *sec = pl + 1 + pl[0];
        if (pll <= 16) return;
        int slen = ((sec[1] & 0xF) << 8) | sec[2];
        int pilen = ((sec[10] & 0xF) << 8) | sec[11];
        int pp = 12 + pilen, apid = -1;
        int lim = 3 + slen - 4;
        if (lim > pll) lim = pll;
        while (pp + 5 <= lim) {
            int st = sec[pp];
            int ep = ((sec[pp + 1] & 0x1F) << 8) | sec[pp + 2];
            int el = ((sec[pp + 3] & 0xF) << 8) | sec[pp + 4];
            if (st == 0x1B && d->vpid < 0) d->vpid = ep;   /* H.264 */
            if (st == 0x0F) apid = ep;                     /* AAC */
            pp += 5 + el;
        }
        d->apid = apid;
        fprintf(stderr, "demux: PMT vpid=%d apid=%d\n", d->vpid, apid);
        return;
    }
    if (pid != d->vpid) return;            /* video only for now */

    if (pusi && d->in_pes)                 /* new PUSI: close previous */
        pes_close(d, cb, ctx);
    if (pusi) {
        d->in_pes = 1;
        d->peslen = 0;
    }
    if (d->in_pes && d->peslen + pll <= DEMUX_PES_CAP) {
        memcpy(d->pes + d->peslen, pl, pll);
        d->peslen += pll;
    } else if (d->in_pes) {
        d->peslen = 0;                     /* oversize: drop PES */
        d->in_pes = 0;
    }
}

int demux_frame(struct demux *d, const unsigned char *frame, int n,
                unsigned short port, au_cb cb, void *ctx)
{
    /* eth → IP → UDP:port → RTP. Fall back to a 0x45 scan if the link
     * layer isn't plain Ethernet II on this radio. */
    const unsigned char *ip = NULL;
    if (n > 34 && frame[12] == 0x08 && frame[13] == 0x00)
        ip = frame + 14;
    else {
        for (int i = 14; i < 34 && i + 20 <= n; i++)
            if (frame[i] == 0x45) { ip = frame + i; break; }
    }
    if (!ip || (ip[0] >> 4) != 4 || ip[9] != 17) return 0;
    int ihl = (ip[0] & 0xF) * 4;
    const unsigned char *udp = ip + ihl;
    int dport = (udp[2] << 8) | udp[3];
    if (dport != port) return 0;
    const unsigned char *r = udp + 8;
    int rn = n - (r - frame);
    if (rn < 12 || (r[0] >> 6) != 2) return 0;

    /* RTP header (extensions skipped), sequence tracking */
    int hl = 12 + 4 * (r[0] & 0xF);
    if (r[0] & 0x10) {                     /* RTP extension */
        if (hl + 4 <= rn)
            hl += 4 + 4 * ((r[hl + 2] << 8) | r[hl + 3]);
    }
    if (hl >= rn) return 0;
    unsigned seq = (r[2] << 8) | r[3];
    if (d->rtp_last) {
        unsigned lost = (seq - d->rtp_last - 1) & 0xFFFF;
        if (lost < 0x8000) d->rtp_lost += lost;
    }
    d->rtp_last = seq;
    d->rtp_cnt++;

    const unsigned char *p = r + hl;
    int plen = rn - hl;
    for (int o = 0; o + 188 <= plen; o += 188)
        ts_packet(d, p + o, cb, ctx);
    return 1;
}

long demux_flush(struct demux *d, au_cb cb, void *ctx)
{
    long pend = d->esw - d->esr;
    if (pend <= 8) {
        /* Nothing split-worthy: discard, so the window can't fill with
         * a tail that will never get a start code. */
        d->esr = d->esw = 0;
        return 0;
    }
    cb(ctx, d->es + d->esr, pend);
    d->esr = d->esw = 0;
    d->au_cnt++;
    return pend;
}
