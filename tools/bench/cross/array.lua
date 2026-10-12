local n = 200000
local arr = {}
local len = 0
local i = 0
while i < n do
  len = len + 1
  arr[len] = i * 2
  i = i + 1
end
local sum_, r = 0, 0
while r < 5 do
  i = 0
  while i < n do
    sum_ = sum_ + arr[i + 1]
    i = i + 1
  end
  r = r + 1
end
r = 0
while r < 3 do
  i = 0
  while i < n do
    arr[i + 1] = arr[i + 1] + 1
    i = i + 1
  end
  r = r + 1
end
i = 0
while i < n do
  len = len + 1
  arr[len] = i
  i = i + 1
end
print(string.format("%d", sum_))
print(string.format("%d", arr[n]))
print(len)
