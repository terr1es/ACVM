def fib(n):
    if n < 2: return n
    return fib(n-1) + fib(n-2)
acc = 0
for i in range(300000): acc = acc + fib(10)
print(acc)
