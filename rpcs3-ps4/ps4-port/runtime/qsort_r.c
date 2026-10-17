#include "ps4_qsort_r.h"

static void swap_elements(unsigned char* a, unsigned char* b, size_t size)
{
    while (size--)
    {
        const unsigned char value = *a;
        *a++ = *b;
        *b++ = value;
    }
}

static void sift_down(unsigned char* base, size_t root, size_t count, size_t size,
    int (*compare)(const void*, const void*, void*), void* context)
{
    while (root < count / 2)
    {
        size_t child = root * 2 + 1;
        if (child + 1 < count &&
            compare(base + child * size, base + (child + 1) * size, context) < 0)
        {
            ++child;
        }
        if (compare(base + root * size, base + child * size, context) >= 0)
        {
            return;
        }
        swap_elements(base + root * size, base + child * size, size);
        root = child;
    }
}

void rpcs3_ps4_qsort_r(void* data, size_t count, size_t size,
    int (*compare)(const void*, const void*, void*), void* context)
{
    if (count < 2 || !size)
    {
        return;
    }
    unsigned char* base = data;
    // Build a max heap, then place its maximum at the end of each prefix.
    // No allocation or shared comparator state is required.
    for (size_t parent = count / 2; parent;)
    {
        sift_down(base, --parent, count, size, compare, context);
    }
    while (--count)
    {
        swap_elements(base, base + count * size, size);
        sift_down(base, 0, count, size, compare, context);
    }
}
