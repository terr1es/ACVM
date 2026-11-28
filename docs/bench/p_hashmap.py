s = set()
for i in range(200000):
    s.add(i * 7)
hits = 0
for i in range(200000):
    if i * 7 in s:
        hits += 1
print(hits)
