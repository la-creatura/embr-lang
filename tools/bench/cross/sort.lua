local seed = 42
local a = {}
for i = 1, 200000 do
  seed = (seed * 1103515245 + 12345) % 2147483648
  a[i] = seed % 1000000
end
table.sort(a)
print(a[1])
print(a[100001])
print(a[200000])
local total = 0
local i = 1
while i <= 200000 do
  total = total + a[i]
  i = i + 1000
end
print(total)
