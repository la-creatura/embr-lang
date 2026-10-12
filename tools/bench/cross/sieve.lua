local n = 2000000
local flags = {}
for i = 0, n do flags[i] = 1 end
flags[0] = 0
flags[1] = 0
local p = 2
while p * p <= n do
  if flags[p] == 1 then
    local m = p * p
    while m <= n do
      flags[m] = 0
      m = m + p
    end
  end
  p = p + 1
end
local count, i = 0, 0
while i <= n do
  count = count + flags[i]
  i = i + 1
end
print(count)
