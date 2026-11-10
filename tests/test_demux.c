/* test_demux.c — host tests for demux.c. Every vector is synthetic,
 * built byte-by-byte here (no captured stream data): PAT/PMT sections,
 * PES packets, RTP/IP/Ethernet framing. Feed frames, assert the AUs
 * that come back. Build & run:
 *   clang -Wall -Wextra -O2 -o test_demux test_demux.c demux.c && ./test_demux
 */
#include <stdio.h>
#include <string.h>
#include "demux.h"

#define PMT_PID 0x100
#define VPID    0x101
#define APID    0x102
#define PORT    15550

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s (line %d)\n", name, __LINE__); fails++; } \
} while (0)

/* ---- AU recorder ---- */
static unsigned char rec[512 * 1024];
static long rec_n;
static int au_count;
static void rec_au(void *ctx, const unsigned char *au, long len)
{
    (void)ctx;
    memcpy(rec + rec_n, au, len);
    rec_n += len;
    au_count++;
}
static void rec_reset(void) { rec_n = 0; au_count = 0; }

/* ---- synthetic AU payloads: NAL 5 start code + filler ---- */
#define AU_FILL 40
static unsigned char au_a[3 + 1 + AU_FILL], au_b[3 + 1 + AU_FILL],
    au_c[3 + 1 + AU_FILL];
static void au_init(unsigned char *au, unsigned char fill)
{
    au[0] = 0; au[1] = 0; au[2] = 1; au[3] = 5;      /* IDR start code */
    memset(au + 4, fill, AU_FILL);
}

/* ---- TS packet builder: payload-only (afc=1) when full, adaptation-
 * field 0xFF stuffing (afc=3) when short — the wire shape, so short
 * payloads don't leak padding bytes into the PES ---- */
static int ts_payload(unsigned char *t, int pid, int pusi,
                      const unsigned char *pl, int plen)
{
    memset(t, 0xFF, 188);
    t[0] = 0x47;
    t[1] = (pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F);
    t[2] = pid & 0xFF;
    if (plen >= 184) {
        t[3] = 0x10;                                 /* payload only */
        memcpy(t + 4, pl, 184);
    } else {
        t[3] = 0x30;                                 /* AF + payload */
        t[4] = 183 - plen;                           /* AF length */
        t[5] = 0x00;                                 /* AF flags: none */
        memcpy(t + 5 + t[4], pl, plen);
    }
    return 188;
}
static int ts_section(unsigned char *t, int pid, const unsigned char *sec,
                      int slen)
{
    unsigned char pl[64];
    pl[0] = 0;                                       /* pointer_field */
    memcpy(pl + 1, sec, slen);
    return ts_payload(t, pid, 1, pl, slen + 1);
}
static int ts_pes(unsigned char *t, const unsigned char *pes, int plen,
                  int pusi)
{
    return ts_payload(t, VPID, pusi, pes, plen);
}
/* PES header (9B: start code, stream id video, PESlen=0, flags, hdrlen=0)
 * + up to `plen` payload bytes — the wire shape the parser expects. */
static int mk_pes(unsigned char *pes, const unsigned char *payload, int plen)
{
    pes[0] = 0; pes[1] = 0; pes[2] = 1; pes[3] = 0xE0;
    pes[4] = 0; pes[5] = 0;                          /* PES_length: 0 */
    pes[6] = 0x80; pes[7] = 0x00;                    /* flags: no PTS */
    pes[8] = 0;                                      /* header length */
    memcpy(pes + 9, payload, plen);
    return 9 + plen;
}

/* ---- PAT / PMT sections ---- */
static void mk_pat(unsigned char *sec)
{
    memset(sec, 0, 17 + 4);
    sec[0] = 0x00;                                   /* table_id */
    sec[1] = 0xB0; sec[2] = 0x0D;                    /* section_length 13 */
    sec[3] = 0; sec[4] = 1;                          /* transport_stream_id */
    sec[5] = 0xC1;                                   /* version, current */
    sec[8] = 0; sec[9] = 1;                          /* program_number 1 */
    sec[10] = 0xE0 | (PMT_PID >> 8);                 /* reserved + PMT PID */
    sec[11] = PMT_PID & 0xFF;
    /* sec[12..16] unused, sec[17..20] would be CRC — not checked */
}
static void mk_pmt(unsigned char *sec)
{
    memset(sec, 0, 32);
    sec[0] = 0x02;                                   /* table_id: PMT */
    sec[1] = 0xB0; sec[2] = 0x17;                    /* section_length 23 */
    sec[3] = 0; sec[4] = 1;                          /* program_number */
    sec[8] = 0xE0; sec[9] = 0;                       /* PCR PID 0 */
    sec[10] = 0xF0; sec[11] = 0x00;                  /* program_info_length 0 */
    sec[12] = 0x1B;                                  /* H.264 */
    sec[13] = 0xE0 | (VPID >> 8); sec[14] = VPID & 0xFF;
    sec[15] = 0xF0; sec[16] = 0x00;                  /* ES info length 0 */
    sec[17] = 0x0F;                                  /* AAC */
    sec[18] = 0xE0 | (APID >> 8); sec[19] = APID & 0xFF;
    sec[20] = 0xF0; sec[21] = 0x00;
}

/* ---- Ethernet/IP/UDP/RTP framing ---- */
static int mk_frame(unsigned char *f, const unsigned char *payload, int plen,
                    unsigned seq, int dport, int rtp_version_ok)
{
    memset(f, 0, 14 + 20 + 8 + 12);
    f[12] = 0x08; f[13] = 0x00;                      /* ethertype IPv4 */
    unsigned char *ip = f + 14;
    int total = 20 + 8 + 12 + plen;
    ip[0] = 0x45; ip[2] = total >> 8; ip[3] = total & 0xFF;
    ip[8] = 64; ip[9] = 17;                          /* UDP */
    unsigned char *udp = ip + 20;
    udp[2] = dport >> 8; udp[3] = dport & 0xFF;
    unsigned char *r = udp + 8;
    r[0] = rtp_version_ok ? 0x80 : 0x40;             /* version 2 or 1 */
    r[1] = 96;                                       /* payload type */
    r[2] = seq >> 8; r[3] = seq & 0xFF;
    memcpy(r + 12, payload, plen);
    return 14 + total;
}

static struct demux d;
static unsigned char frame[2048], sec[64], pes[DEMUX_PES_CAP], ts[188 * 8];

static void demux_reset(void)
{
    memset(&d, 0, sizeof d);
    d.pmt_pid = d.vpid = d.apid = -1;
    rec_reset();
}

static void feed_pat_pmt(unsigned seq)
{
    int n = 0;
    mk_pat(sec);
    n += ts_section(ts + n, 0, sec, 21);
    mk_pmt(sec);
    n += ts_section(ts + n, PMT_PID, sec, 22);
    int fl = mk_frame(frame, ts, n, seq, PORT, 1);
    demux_frame(&d, frame, fl, PORT, rec_au, NULL);
}

/* one video-pid TS packet: PUSI frames a PES header around `payload`;
 * no PUSI sends it raw, as a continuation of the PES under construction */
static void feed_pes(const unsigned char *payload, int plen, int pusi,
                     unsigned seq)
{
    int tn;
    if (pusi) {
        int pn = mk_pes(pes, payload, plen);
        tn = ts_pes(ts, pes, pn, 1);
    } else {
        tn = ts_pes(ts, payload, plen, 0);
    }
    int fl = mk_frame(frame, ts, tn, seq, PORT, 1);
    demux_frame(&d, frame, fl, PORT, rec_au, NULL);
}

int main(void)
{
    au_init(au_a, 0xAA);
    au_init(au_b, 0xBB);
    au_init(au_c, 0xCC);
    const int aulen = 4 + AU_FILL;

    /* 1. discovery: PAT → PMT → video pid, no AUs yet */
    demux_reset();
    feed_pat_pmt(100);
    CHECK(d.pmt_pid == PMT_PID, "PAT yields PMT pid");
    CHECK(d.vpid == VPID, "PMT yields H.264 video pid");
    CHECK(d.apid == APID, "PMT yields AAC audio pid");
    CHECK(au_count == 0, "no AUs from program tables alone");

    /* 2. AUs are cut on the NEXT picture start: two PES, one emitted,
     *    the second rides the window until the third PES closes it */
    feed_pes(au_a, aulen, 1, 101);                   /* PES1: AU_A */
    CHECK(au_count == 0, "AU not cut before a successor start code");
    feed_pes(au_b, aulen, 1, 102);                   /* PUSI closes PES1 */
    CHECK(au_count == 0, "still one AU in window: no next code yet");
    feed_pes(au_c, aulen, 1, 103);                   /* PUSI closes PES2 */
    CHECK(au_count == 1, "first AU emitted when second completes");
    CHECK(rec_n == aulen && memcmp(rec, au_a, aulen) == 0,
          "emitted AU is exactly AU_A");
    long fl = demux_flush(&d, rec_au, NULL);
    CHECK(fl == aulen && au_count == 2 &&
          memcmp(rec + aulen, au_b, aulen) == 0, "flush emits pending AU_B");
    CHECK(demux_flush(&d, rec_au, NULL) == 0, "second flush is empty");

    /* 3. AU spanning a PES boundary (the wire shape: PES_length=0,
     *    AUs run PUSI→PUSI) — continuation packets have no PUSI */
    demux_reset();
    feed_pat_pmt(200);
    feed_pes(au_a, aulen, 1, 201);                   /* PES1: AU_A */
    feed_pes(au_b, 12, 0, 202);                      /* cont: AU_B head */
    feed_pes(au_b + 12, aulen - 12, 1, 203);         /* PUSI closes PES1 */
    CHECK(au_count == 1 && rec_n == aulen &&
          memcmp(rec, au_a, aulen) == 0, "AU_A emitted after boundary close");
    feed_pes(au_c, aulen, 1, 204);                   /* PUSI closes AU_B's tail */
    CHECK(au_count == 1, "AU_B pending until a successor start code");
    CHECK(demux_flush(&d, rec_au, NULL) == aulen && au_count == 2 &&
          memcmp(rec + aulen, au_b, aulen) == 0,
          "AU_B reassembled across the PES boundary");

    /* 4. sparse flush: tail without a successor code (static screen) */
    demux_reset();
    feed_pat_pmt(300);
    feed_pes(au_a, aulen, 1, 301);
    feed_pes(au_b, aulen, 1, 302);
    fl = demux_flush(&d, rec_au, NULL);
    CHECK(fl == aulen && au_count == 1, "sparse flush cuts the pending AU");
    CHECK(d.esr == 0 && d.esw == 0, "flush empties the window");

    /* 5. frame filtering: wrong port, bad RTP version, no ethertype
     *    (0x45 fallback must still find the IP header) */
    demux_reset();
    {
        int tn = ts_pes(ts, pes, mk_pes(pes, au_a, aulen), 1);
        int fl2 = mk_frame(frame, ts, tn, 400, PORT + 1, 1);
        CHECK(demux_frame(&d, frame, fl2, PORT, rec_au, NULL) == 0,
              "wrong UDP port is skipped");
        fl2 = mk_frame(frame, ts, tn, 401, PORT, 0);
        CHECK(demux_frame(&d, frame, fl2, PORT, rec_au, NULL) == 0,
              "RTP version 1 is skipped");
        CHECK(au_count == 0 && d.rtp_cnt == 0, "skipped frames count no RTP");
        /* strip the ethertype, keep a raw-IP-looking prefix */
        mk_frame(frame, ts, tn, 402, PORT, 1);
        frame[12] = 0; frame[13] = 0;                /* not 0x0800 */
        frame[14] = 0x45;                            /* fallback scan hit */
        CHECK(demux_frame(&d, frame, 2048, PORT, rec_au, NULL) == 1,
              "0x45 fallback scan finds the IP header");
        CHECK(d.rtp_cnt == 1, "fallback frame consumed as RTP");
    }

    /* 6. RTP sequence-loss accounting */
    demux_reset();
    {
        int tn = ts_pes(ts, pes, 0, 0);              /* bare TS, no PES */
        int f1 = mk_frame(frame, ts, tn, 500, PORT, 1);
        demux_frame(&d, frame, f1, PORT, rec_au, NULL);
        int f2 = mk_frame(frame, ts, tn, 502, PORT, 1);   /* gap: 501 lost */
        demux_frame(&d, frame, f2, PORT, rec_au, NULL);
        int f3 = mk_frame(frame, ts, tn, 40000, PORT, 1); /* huge jump: not
                                                       counted (reorder) */
        demux_frame(&d, frame, f3, PORT, rec_au, NULL);
        CHECK(d.rtp_lost == 1, "one lost packet counted");
        CHECK(d.rtp_cnt == 3, "all three frames counted");
    }

    /* 7. malformed TS: bad sync byte, truncated tail — no crash, no AU */
    demux_reset();
    feed_pat_pmt(600);
    {
        unsigned char mix[188 * 3];
        int n = 0;
        n += ts_pes(mix + n, pes, mk_pes(pes, au_a, aulen), 1);
        mix[n] = 0x00;                               /* bad sync */
        n += 188;
        n += ts_pes(mix + n, pes, mk_pes(pes, au_b, aulen), 1);
        int fl3 = mk_frame(frame, mix, n + 100, 601, PORT, 1); /* +trunc */
        (void)demux_frame(&d, frame, fl3, PORT, rec_au, NULL);
        CHECK(au_count == 0, "corrupt sync byte: packet lost, PES intact");
        feed_pes(au_c, aulen, 1, 602);
        CHECK(au_count == 1 && memcmp(rec, au_a, aulen) == 0,
              "stream recovers after malformed packet");
    }

    /* 8. oversize PES dropped whole; the next PES still parses */
    demux_reset();
    feed_pat_pmt(700);
    {
        memset(pes + 9, 0xEE, DEMUX_PES_CAP - 9);    /* one giant PES */
        int tn = ts_pes(ts, pes, 9 + 300, 1);
        int fl4 = mk_frame(frame, ts, tn, 701, PORT, 1);
        demux_frame(&d, frame, fl4, PORT, rec_au, NULL);
        int sent = 0;                                /* continuation TS */
        while (sent < DEMUX_PES_CAP - 9) {
            int n = 0;
            for (int k = 0; k < 7 && sent < DEMUX_PES_CAP - 9; k++) {
                int chunk = DEMUX_PES_CAP - 9 - sent;
                if (chunk > 184) chunk = 184;
                n += ts_pes(ts + n, pes + 9 + sent, chunk, 0);
                sent += chunk;
            }
            fl4 = mk_frame(frame, ts, n, 702, PORT, 1);
            demux_frame(&d, frame, fl4, PORT, rec_au, NULL);
        }
        CHECK(d.in_pes == 0, "oversize PES is dropped");
        feed_pes(au_a, aulen, 1, 800);               /* fresh PUSI PES */
        feed_pes(au_b, aulen, 1, 801);
        CHECK(d.in_pes == 1, "parser takes the next PES after the drop");
    }

    printf(fails ? "\n%d FAILURES\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
