def make_counter():
    n = 0
    def counter():
        nonlocal n
        n = n + 1
        return n
    return counter
c = make_counter()
i = 0
while i < 500000:
    c()
    i = i + 1
print(c())
