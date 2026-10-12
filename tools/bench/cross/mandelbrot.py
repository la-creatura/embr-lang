def inside(cr, ci):
    zr = 0.0
    zi = 0.0
    k = 0
    while k < 80:
        t = zr * zr - zi * zi + cr
        zi = 2.0 * zr * zi + ci
        zr = t
        if zr * zr + zi * zi > 4.0:
            return 0
        k = k + 1
    return 1
count = 0
py = 0
while py < 160:
    px = 0
    while px < 160:
        count = count + inside(px * 3.0 / 160.0 - 2.0, py * 2.0 / 160.0 - 1.0)
        px = px + 1
    py = py + 1
print(count)
