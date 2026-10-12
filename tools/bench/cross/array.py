n = 200000
arr = []
i = 0
while i < n:
    arr.append(i * 2)
    i = i + 1
sum_ = 0
r = 0
while r < 5:
    i = 0
    while i < n:
        sum_ = sum_ + arr[i]
        i = i + 1
    r = r + 1
r = 0
while r < 3:
    i = 0
    while i < n:
        arr[i] = arr[i] + 1
        i = i + 1
    r = r + 1
i = 0
while i < n:
    arr.append(i)
    i = i + 1
print(sum_)
print(arr[n - 1])
print(len(arr))
