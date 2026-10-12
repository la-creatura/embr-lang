-- table.concat would be the fast way; this does what the other languages do, one concatenation at a time
local s, i = "", 0
while i < 100000 do
  s = s .. "ab"
  i = i + 1
end
print(#s)
