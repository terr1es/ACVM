def find_sub(text, sub):
    n, m = len(text), len(sub)
    for i in range(n - m + 1):
        j = 0
        while j < m and text[i + j] == sub[j]:
            j += 1
        if j == m:
            return i
    return -1


text = "the quick brown fox jumps over the lazy dog 0123456789 the quick brown fox jumps over the lazy dog 0123456789 the quick brown fox jumps over the lazy dog 0123456789 the quick brown fox jumps over the lazy dog 0123456789"
needle = "lazy dog"
acc = 0
for _ in range(20000):
    acc += find_sub(text, needle)
print(acc)
