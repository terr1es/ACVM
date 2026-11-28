def classify(x):
    if x < 0:
        return -1
    if x % 2 == 0:
        if x % 3 == 0:
            return 2
        return 1
    if x % 5 == 0:
        return 3
    return 4


acc = 0
for i in range(5000000):
    acc += classify(i % 1000)
print(acc)
