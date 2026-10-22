#!/usr/bin/env python3
"""Draw the Onion app icon: a small spectrum trace over a waterfall."""
import math
import random
import sys

from PIL import Image, ImageDraw

W = H = 74
img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([1, 1, W - 2, H - 2], radius=12, fill=(14, 20, 28, 255))

random.seed(7)
# waterfall in the lower half
for y in range(40, 66):
    for x in range(8, 66):
        hot = math.exp(-((x - 46) / 3.0) ** 2) * (1 if (y // 3) % 2 == 0 else 0.6)
        v = min(1.0, 0.15 + 0.1 * random.random() + hot)
        d.point((x, y), fill=(int(40 + 215 * v), int(30 + 170 * v * v), int(90 - 60 * v), 255))

# spectrum trace in the upper half
pts = []
for x in range(8, 67):
    level = 0.25 + 0.08 * random.random() + 0.9 * math.exp(-((x - 46) / 2.5) ** 2) \
        + 0.35 * math.exp(-((x - 22) / 6.0) ** 2)
    pts.append((x, 36 - level * 24))
d.line(pts, fill=(61, 220, 132, 255), width=2)
img.save(sys.argv[1])
