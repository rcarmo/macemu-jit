#!/usr/bin/env python3
"""Capture a VNC screenshot from BasiliskII. Usage: vnc-screenshot.py [output.png] [host:port]"""
import socket, struct, sys
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else '/workspace/tmp/vnc-screenshot.png'
addr = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1:5999'
host, port = addr.rsplit(':', 1)

sock = socket.socket()
sock.settimeout(10)
sock.connect((host, int(port)))

# RFB 3.8 handshake
sock.recv(12)
sock.send(b'RFB 003.008\n')
n = struct.unpack('B', sock.recv(1))[0]
sock.recv(n)
sock.send(bytes([1]))  # No auth
sock.recv(4)
sock.send(bytes([1]))  # Shared

# ServerInit
si = sock.recv(24)
w, h = struct.unpack('>HH', si[:4])
name_len = struct.unpack('>I', si[20:24])[0]
sock.recv(name_len)

# Request full framebuffer (raw encoding, server native format)
sock.send(struct.pack('>BBHHHH', 3, 0, 0, 0, w, h))

# Read FramebufferUpdate
assert sock.recv(1) == b'\x00'
sock.recv(1)
n_rects = struct.unpack('>H', sock.recv(2))[0]

pixels = bytearray(w * h * 4)
for _ in range(n_rects):
    rx, ry, rw, rh, enc = struct.unpack('>HHHHi', sock.recv(12))
    assert enc == 0, f"Unsupported encoding {enc}"
    data = b''
    needed = rw * rh * 4
    while len(data) < needed:
        data += sock.recv(needed - len(data))
    for row in range(rh):
        s = row * rw * 4
        d = ((ry + row) * w + rx) * 4
        pixels[d:d + rw * 4] = data[s:s + rw * 4]

sock.close()

img = Image.frombytes('RGBX', (w, h), bytes(pixels)).convert('RGB')
img.save(out)
colors = len(set(list(img.getdata())))
print(f"{out}: {w}x{h}, {colors} colors")
