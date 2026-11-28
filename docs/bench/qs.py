import time
THRESHOLD = 20

def fill(arr, count, seed):
    state = seed
    for i in range(count):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        arr[i] = ((state >> 16) % 1000)

def insertion_sort(a, lo, span):
    i = 1
    while i <= span:
        key = a[lo + i]
        scan = lo + i - 1
        while scan >= lo:
            v = a[scan]
            if v <= key:
                break
            a[scan + 1] = v
            scan -= 1
        a[scan + 1] = key
        i += 1

def quicksort(a, lo, hi):
    while lo < hi:
        span = hi - lo
        if span < THRESHOLD:
            insertion_sort(a, lo, span)
            return
        mid = lo + span // 2
        x, y, z = a[lo], a[mid], a[hi]
        if x < y:
            pivot = y if y < z else (z if x < z else x)
        else:
            pivot = x if x < z else (z if y < z else y)
        left, right = lo, hi
        while True:
            while a[left] < pivot: left += 1
            while a[right] > pivot: right -= 1
            if left >= right: break
            a[left], a[right] = a[right], a[left]
            left += 1; right -= 1
        if right - lo < hi - right:
            quicksort(a, lo, right); lo = right + 1
        else:
            quicksort(a, right + 1, hi); hi = right

def inversions(a, n):
    c = 0
    for i in range(1, n):
        if a[i-1] > a[i]: c += 1
    return c

n = 50000
data = [0]*n
fill(data, n, 12345)
before = inversions(data, n)
t0 = time.perf_counter()
quicksort(data, 0, n-1)
el = time.perf_counter() - t0
after = inversions(data, n)
print(f"values:            {n}")
print(f"inversions before: {before}")
print(f"inversions after:  {after}" + ("  (sorted)" if after==0 else "  (NOT sorted)"))
print(f"smallest:          {data[0]}")
print(f"largest:           {data[n-1]}")
print(f"sort time:         {el*1000:.1f} ms")
