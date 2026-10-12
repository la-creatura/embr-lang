seed = 42
a = []
i = 0
while i < 200000:
    seed = (seed * 1103515245 + 12345) % 2147483648
    a.append(seed % 1000000)
    i = i + 1
s = sorted(a)
print(s[0])
print(s[100000])
print(s[199999])
total = 0
i = 0
while i < 200000:
    total = total + s[i]
    i = i + 1000
print(total)
