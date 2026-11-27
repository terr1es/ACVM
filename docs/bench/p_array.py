buf = list(range(1000)); total = 0
for r in range(2000):
    for i in range(1000): total = total + buf[i]
print(total)
