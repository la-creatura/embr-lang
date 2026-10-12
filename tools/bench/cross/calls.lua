local function add(a, b) return a + b end
local function twice(x) return add(x, x) end
local acc, i = 0, 0
while i < 600000 do
  acc = add(acc, twice(i))
  i = i + 1
end
print(string.format("%d", acc))
