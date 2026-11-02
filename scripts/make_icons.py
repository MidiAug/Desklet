"""Build code-drawn native icons; no runtime imaging dependency."""
from pathlib import Path
from PIL import Image, ImageDraw

target = Path(__file__).resolve().parents[1] / 'assets'
for name, color in [('desklet', '#3a5eed'), ('single', '#2c9165'), ('multi', '#7370e7')]:
    im = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((8, 8, 248, 248), radius=64, fill=color)
    if name == 'desklet':
        d.rounded_rectangle((49, 62, 207, 169), radius=15, outline='white', width=12)
        d.line((128, 171, 128, 197), fill='white', width=12)
        d.line((91, 199, 165, 199), fill='white', width=12)
        d.line((128, 71, 128, 158), fill='#c7d2ff', width=8)
    else:
        if name == 'multi':
            d.rounded_rectangle((82, 52, 207, 165), radius=14, outline='#dadaff', width=10)
        d.rounded_rectangle((48, 83 if name == 'multi' else 67, 181 if name == 'multi' else 207, 202 if name == 'multi' else 190), radius=15, fill=color, outline='white', width=12)
        d.line((54, 117 if name == 'multi' else 105, 176 if name == 'multi' else 200, 117 if name == 'multi' else 105), fill='white', width=9)
    im.save(target / f'{name}.ico', sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
