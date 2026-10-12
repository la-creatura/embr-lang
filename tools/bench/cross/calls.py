def add(a, b):
    return a + b
def twice(x):
    return add(x, x)
acc = 0
i = 0
while i < 600000:
    acc = add(acc, twice(i))
    i = i + 1
print(acc)
