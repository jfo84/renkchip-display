# Build a minimal valid EDID 1.3 with 1920x1080p60 preferred timing (CEA-861 VIC16)
import struct

edid = bytearray(128)

# header
edid[0:8] = bytes.fromhex('00ffffffffffff00')
# manufacturer "QMD" generic (Renkchip-ish), product 0x0001
edid[8] = (ord('Q')-64) << 2 | (ord('M')-64) >> 3
edid[9] = ((ord('M')-64) & 0x7) << 5 | (ord('D')-64) & 0x1f
edid[10:12] = struct.pack('<H', 1)
edid[16] = 0x22  # week 34
edid[17] = 0x1e  # year 2024 (1990+30)
edid[18] = 1     # EDID 1.3
edid[19] = 3     # digital
edid[20] = 0x78  # 1200mm diagonal... keep small: leave
# video input def byte 20: digital, DVI... 0x80 = digital
edid[20] = 0x80
# established timings: set 1280x1024@75 (bit15) etc. leave zeros except standard
# standard timings: 8 x 2 bytes at 38-53
# use 0x0101 (unused) for all
for i in range(38, 54):
    edid[i] = 0x01
# descriptor 1: preferred detailed timing 1920x1080p60
d = bytearray(18)
pixclk = 14850  # 148.50 MHz in 10kHz
d[0] = pixclk & 0xff
d[1] = pixclk >> 8
hactive, hblank = 1920, 280
vactive, vblank = 1080, 45
d[2] = hactive & 0xff
d[3] = hblank & 0xff
d[4] = ((hactive >> 8) & 0xf) << 4 | ((hblank >> 8) & 0xf)
d[5] = vactive & 0xff
d[6] = vblank & 0xff
d[7] = ((vactive >> 8) & 0xf) << 4 | ((vblank >> 8) & 0xf)
hfront, hsync = 88, 44
vfront, vsync = 4, 5
d[8] = hfront & 0xff
d[9] = hsync & 0xff
d[10] = (vfront & 0xf) << 4 | (vsync & 0xf)
d[11] = 0  # no upper bits
# image size 600x340 mm
him, vim = 600, 340
d[12] = him & 0xff
d[13] = vim & 0xff
d[14] = ((him >> 8) & 0xf) << 4 | ((vim >> 8) & 0xf)
d[15] = 0x1e  # no interlace, digital separate sync
edid[54:72] = d
# descriptor 2: monitor name "1080P-SINK"
d2 = bytearray(18)
d2[0:2] = b'\x00\x00'
d2[2] = 0
d2[3] = 0xfc  # monitor name
d2[4] = 0x00
name = b'1080P-SINK'
d2[5:5+len(name)] = name
d2[5+len(name)] = 0x0a
edid[72:90] = d2
# descriptor 3: range limits 0x50-0x62 vertical... keep minimal (0xFD)
d3 = bytearray(18)
d3[0:2] = b'\x00\x00'
d3[3] = 0xfd
d3[5:10] = bytes([50, 60, 30, 75, 255])  # vrate 50-60, hrate 30-75k... max pixel 255*10MHz? use 0xff
edid[90:108] = d3
# descriptor 4: zero (unused)
# extensions
edid[126] = 0
# checksum
edid[127] = (256 - (sum(edid) & 0xff)) & 0xff

open('edid_1080.bin', 'wb').write(edid)
print('wrote edid_1080.bin, checksum byte 0x%02x, sum-check %s' % (edid[127], sum(edid) & 0xff == 0))
print('DTD:', edid[54:72].hex())