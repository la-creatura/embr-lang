n = 100000
m = {}
i = 0
while i < n:
    m["k" + str(i)] = i
    i = i + 1
total = 0
i = 0
while i < n:
    total = total + m["k" + str(i)]
    i = i + 1
i = 0
while i < n:
    m["k" + str(i)] = i * 2
    i = i + 1
print(total)
print(m["k99999"])
