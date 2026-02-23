"""Generate a simple orange 'C' icon for CLTray."""
from PIL import Image, ImageDraw

def draw_c(img, size):
    draw = ImageDraw.Draw(img)
    cx, cy = size // 2, size // 2
    r = int(size * 0.38)
    thick = max(int(size * 0.16), 2)
    bbox = [cx - r, cy - r, cx + r, cy + r]
    draw.arc(bbox, start=30, end=330, fill=(232, 140, 50, 255), width=thick)

img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
draw_c(img, 256)
img.save("app.ico", format="ICO", sizes=[(16, 16), (32, 32), (48, 48)])
print("Generated app.ico")
