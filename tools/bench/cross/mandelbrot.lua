local function inside(cr, ci)
  local zr, zi, k = 0.0, 0.0, 0
  while k < 80 do
    local t = zr * zr - zi * zi + cr
    zi = 2.0 * zr * zi + ci
    zr = t
    if zr * zr + zi * zi > 4.0 then return 0 end
    k = k + 1
  end
  return 1
end
local count, py = 0, 0
while py < 160 do
  local px = 0
  while px < 160 do
    count = count + inside(px * 3.0 / 160.0 - 2.0, py * 2.0 / 160.0 - 1.0)
    px = px + 1
  end
  py = py + 1
end
print(count)
