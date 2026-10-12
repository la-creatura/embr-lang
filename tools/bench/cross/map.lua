local n = 100000
local m = {}
local i = 0
while i < n do
  m["k" .. i] = i
  i = i + 1
end
local total = 0
i = 0
while i < n do
  total = total + m["k" .. i]
  i = i + 1
end
i = 0
while i < n do
  m["k" .. i] = i * 2
  i = i + 1
end
print(string.format("%d", total))
print(string.format("%d", m["k99999"]))
