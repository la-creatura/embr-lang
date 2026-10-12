n = 2000000
flags = [1] * (n + 1)
flags[0] = 0
flags[1] = 0
p = 2
while p * p <= n:
    if flags[p] == 1:
        m = p * p
        while m <= n:
            flags[m] = 0
            m = m + p
    p = p + 1
count = 0
i = 0
while i <= n:
    count = count + flags[i]
    i = i + 1
print(count)
