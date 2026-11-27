s = 0
for i in range(5000000): s = s + (i % 7) * 2 - (i % 3)
print(s)
