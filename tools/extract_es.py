#!/usr/bin/env python3
"""extract_es.py — wfdcap pcapng → RTP (PT 33) → TS → PES → H.264 ES (+ AAC).

pktmon etl2pcap duplicates every packet 2x; dedup by (seq, payload).
PES lengths are zero for video, so video ES = concatenation of PES payloads.
"""
import hashlib
import struct
import sys


def parse_pcapng(buf):
    """Yield captured packet bytes from Enhanced Packet Blocks."""
    off, n = 0, len(buf)
    while off + 12 <= n:
        btype, blen = struct.unpack_from("<II", buf, off)
        if btype == 0x0A0D0D0A:  # SHB, endian marker
            off += blen
            continue
        if blen < 12 or off + blen > n:
            break
        if btype == 6:  # EPB
            caplen, = struct.unpack_from("<I", buf, off + 20)
            yield buf[off + 28 : off + 28 + caplen]
        off += blen


def ip_offset(pkt):
    """pktmon etl2pcap writes a pseudo-header + LLC/SNAP; find the IPv4 start."""
    snap = pkt.find(b"\xaa\x03\x00\x00\x00\x08\x00")
    if snap != -1:
        return snap + 7
    for i in range(14, min(len(pkt) - 20, 40)):
        if pkt[i] & 0xF0 == 0x40 and (pkt[i] & 0xF) >= 5:
            tlen = struct.unpack_from(">H", pkt, i + 2)[0]
            if 20 <= tlen <= len(pkt) - i:
                return i
    return None


def rtp_payload(pkt):
    """pktmon/LLC-SNAP/IPv4/UDP → RTP payload if PT==33, else None."""
    ip0 = ip_offset(pkt)
    if ip0 is None or (pkt[ip0] >> 4) != 4 or pkt[ip0 + 9] != 17:
        return None
    ihl = (pkt[ip0] & 0xF) * 4
    udp = ip0 + ihl
    sport, dport = struct.unpack_from(">HH", pkt, udp)
    if 15550 not in (sport, dport):
        return None
    rtp = udp + 8
    if len(pkt) < rtp + 12 or (pkt[rtp] >> 6) != 2:
        return None
    if (pkt[rtp + 1] & 0x7F) != 33:
        return None
    seq = struct.unpack_from(">H", pkt, rtp + 2)[0]
    hdr = 12 + 4 * (pkt[rtp] & 0xF)
    if pkt[rtp] & 0x10:  # extension
        hdr += 4 + 4 * struct.unpack_from(">H", pkt, rtp + hdr + 2)[0]
    return seq, pkt[rtp + hdr :]


def collect_payloads(buf):
    """Dedup + reorder RTP payloads by sequence number with wraparound."""
    seen, hi = {}, -1
    for pkt in parse_pcapng(buf):
        got = rtp_payload(pkt)
        if got is None:
            continue
        seq, payload = got
        key = (seq, hashlib.md5(payload).hexdigest())
        if key in seen:
            continue
        seen[key] = (seq, payload)
    pkts = sorted(seen.values(), key=lambda sp: sp[0])
    # unwrap: rotate so the largest gap sits at the end
    if len(pkts) > 1 and pkts[0][0] > pkts[-1][0]:
        cut = max(range(len(pkts)), key=lambda i: pkts[i][0])
        pkts = pkts[cut + 1 :] + pkts[: cut + 1]
    return [p for _, p in pkts]


def section_of(ts, pid):
    """First PUSI packet of pid → PSI section bytes (pointer field applied)."""
    for i in range(0, len(ts) - 188, 188):
        if ts[i] != 0x47 or ((ts[i + 1] & 0x1F) << 8 | ts[i + 2]) != pid:
            continue
        if not ts[i + 1] & 0x40:
            continue
        return ts[i + 5 + ts[i + 4] :]
    return None


def parse_pmt(ts, pat_pid=0):
    """PAT/PMT walk → {stream_type: pid}. H.264 0x1B, AAC 0x0F."""
    pat = section_of(ts, pat_pid)
    if pat is None:
        return {}
    pmt_pid = (pat[10] << 8 | pat[11]) & 0x1FFF  # first program entry
    pmt = section_of(ts, pmt_pid)
    if pmt is None:
        return {}
    slen = (pmt[1] & 0xF) << 8 | pmt[2]
    pilen = (pmt[10] & 0xF) << 8 | pmt[11]
    pids = {}
    p = 12 + pilen
    while p + 5 <= 3 + slen - 4 and p + 5 <= len(pmt):
        stype = pmt[p]
        epid = (pmt[p + 1] & 0x1F) << 8 | pmt[p + 2]
        eslen = (pmt[p + 3] & 0xF) << 8 | pmt[p + 4]
        pids[stype] = epid
        p += 5 + eslen
    return pids


def pes_payloads(ts, pid):
    """Assemble PES payloads for a PID; video PES length is 0 → run to next PUSI."""
    cur = None
    for i in range(0, len(ts) - 188, 188):
        if ts[i] != 0x47 or ((ts[i + 1] & 0x1F) << 8 | ts[i + 2]) != pid:
            continue
        afc = ts[i + 3] >> 4 & 3
        p = i + 4
        if afc & 2:
            p += 1 + ts[p]
        if not afc & 1 or p >= i + 188:
            continue
        if ts[i + 1] & 0x40:  # PUSI: PES start
            if cur is not None:
                yield bytes(cur)
            cur = bytearray()
        if cur is not None:
            cur += ts[p : i + 188]
    if cur is not None:
        yield bytes(cur)


def es_from_pes(pes_chunks):
    """Strip PES headers; concatenate ES."""
    es = bytearray()
    for chunk in pes_chunks:
        if chunk[:3] != b"\x00\x00\x01":
            continue
        hdrlen = 9 + chunk[8]
        es += chunk[hdrlen:]
    return bytes(es)


def nal_scan(es):
    """Count NAL types of interest via start codes."""
    counts, starts = {}, []
    i = es.find(b"\x00\x00\x01")
    while i != -1:
        starts.append(i + 3)
        i = es.find(b"\x00\x00\x01", i + 3)
    for a, b in zip(starts, starts[1:] + [len(es)]):
        if b - a < 1:
            continue
        t = es[a] & 0x1F
        counts[t] = counts.get(t, 0) + 1
    return counts


def main():
    src, out_base = sys.argv[1], sys.argv[2]
    buf = open(src, "rb").read()
    payloads = collect_payloads(buf)
    ts = b"".join(payloads)
    print(f"rtp packets {len(payloads)}, ts bytes {len(ts)}")
    pids = parse_pmt(ts)
    print(f"pmt: {pids or 'not found, using defaults'}")
    vpid = pids.get(0x1B, 0x11)
    apid = pids.get(0x0F, 0x12)
    vpes = list(pes_payloads(ts, vpid))
    ves = es_from_pes(vpes)
    open(out_base + ".h264", "wb").write(ves)
    counts = nal_scan(ves)
    sps = counts.get(7, 0)
    pps = counts.get(8, 0)
    idr = counts.get(5, 0)
    nonidr = counts.get(1, 0)
    print(f"video pid {vpid:#04x}: pes {len(vpes)}, es {len(ves)} B, "
          f"nal sps {sps} pps {pps} idr {idr} nonidr {nonidr}")
    apes = list(pes_payloads(ts, apid))
    aes = es_from_pes(apes)
    open(out_base + ".aac", "wb").write(aes)
    print(f"audio pid {apid:#04x}: pes {len(apes)}, es {len(aes)} B")


if __name__ == "__main__":
    main()
