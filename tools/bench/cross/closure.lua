local function make_counter()
  local n = 0
  return function()
    n = n + 1
    return n
  end
end
local c = make_counter()
local i = 0
while i < 500000 do
  c()
  i = i + 1
end
print(string.format("%d", c()))
