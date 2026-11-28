//  quicksort_max.cpp — the same algorithm as examples/quicksort_max.ax, so that the
//  three-engine table in the README compares one algorithm rather than three.
//
//  Faithful to the Axiom version, which matters more than it looks: a C++ quicksort
//  written the idiomatic way -- `std::sort`, or a different partition scheme, or a
//  different insertion threshold -- would be a different algorithm and the ratio would
//  mean nothing. Every choice below is the one `quicksort_max.ax` makes, including the
//  threshold of 20, median-of-three by comparison rather than by swapping, Hoare
//  partitioning with the pivot value stopping both scans, and recursing into the smaller
//  side while iterating on the larger.
//
//  The cross-check is the printed inversion counts: they match the Axiom run exactly, which
//  is what makes it legitimate to put the two times in one table.
//
//  Takes the element count from the command line, defaulting to 50 000, so that the same
//  binary produces every column of the multi-size table in the README.
//
//  Build:  clang++ -O2 -std=c++20 -o qs.exe qs.cpp
//          clang++ -O0 -std=c++20 -o qs0.exe qs.cpp
//  Run:    ./qs.exe 200000

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kThreshold = 20;

// The same linear congruential generator, in the same order. `unsigned int` is 32-bit on
// every platform this is built for, and the masking is what the Axiom version gets from
// `unsigned int` arithmetic wrapping.
void fillPseudoRandom(int* out, int count, std::uint32_t seed) {
    std::uint32_t state = seed;
    for (int i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<int>(state >> 16) % 1000;
    }
}

void insertionSort(int* base, int span) {
    int i = 1;
    while (i <= span) {
        int* hole = base + i;
        const int key = *hole;
        int* scan = hole - 1;
        while (scan >= base) {
            const int v = *scan;
            if (v <= key) {
                break;
            }
            *(scan + 1) = v;
            scan--;
        }
        *(scan + 1) = key;
        i++;
    }
}

void quicksort(int* lo, int* hi) {
    while (lo < hi) {
        const int span = static_cast<int>(hi - lo);
        if (span < kThreshold) {
            insertionSort(lo, span);
            return;
        }

        int* mid = lo + span / 2;
        const int a = *lo;
        const int b = *mid;
        const int c = *hi;
        int pivot = 0;
        if (a < b) {
            if (b < c) { pivot = b; }
            else if (a < c) { pivot = c; }
            else { pivot = a; }
        } else {
            if (a < c) { pivot = a; }
            else if (b < c) { pivot = c; }
            else { pivot = b; }
        }

        int* left = lo;
        int* right = hi;
        while (true) {
            while (*left < pivot) { left++; }
            while (*right > pivot) { right--; }
            if (left >= right) { break; }
            const int t = *left;
            *left = *right;
            *right = t;
            left++;
            right--;
        }

        if (right - lo < hi - right) {
            quicksort(lo, right);
            lo = right + 1;
        } else {
            quicksort(right + 1, hi);
            hi = right;
        }
    }
}

int countInversions(const int* data, int count) {
    int inversions = 0;
    for (int i = 1; i < count; ++i) {
        if (data[i - 1] > data[i]) {
            inversions++;
        }
    }
    return inversions;
}

} // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 50000;
    if (count <= 0) {
        std::fprintf(stderr, "usage: %s [element-count]\n", argv[0]);
        return 2;
    }
    std::vector<int> data(static_cast<std::size_t>(count));

    fillPseudoRandom(data.data(), count, 12345u);

    const int before = countInversions(data.data(), count);
    quicksort(data.data(), data.data() + count - 1);
    const int after = countInversions(data.data(), count);

    std::printf("values:            %d\n", count);
    std::printf("inversions before: %d\n", before);
    std::printf("inversions after:  %d\n", after);
    std::printf(after == 0 ? "  (sorted)\n" : "  (NOT sorted)\n");
    std::printf("smallest:          %d\n", data[0]);
    std::printf("largest:           %d\n", data[count - 1]);
    return 0;
}
