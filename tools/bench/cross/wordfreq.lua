local words = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau", "upsilon"}
local seed = 12345
local parts = {}
for i = 1, 200000 do
  seed = (seed * 1103515245 + 12345) % 2147483648
  parts[i] = words[seed % 20 + 1]
end
local text = table.concat(parts, " ")
local counts, distinct = {}, 0
for w in string.gmatch(text, "[^ ]+") do
  if counts[w] then counts[w] = counts[w] + 1 else counts[w] = 1; distinct = distinct + 1 end
end
print(distinct)
print(counts["alpha"])
print(counts["upsilon"])
print(#text)
