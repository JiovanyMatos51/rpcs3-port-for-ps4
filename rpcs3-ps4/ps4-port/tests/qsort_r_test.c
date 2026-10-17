#include "ps4_qsort_r.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct record { int key; unsigned char payload[13]; };
static int compare_records(const void* a, const void* b, void* context)
{
    const struct record* x = a;
    const struct record* y = b;
    const int direction = *(const int*)context;
    return ((x->key > y->key) - (x->key < y->key)) * direction;
}

int main(void)
{
    struct record data[257];
    uint32_t state = 12345;
    const int directions[] = {1, -1};
    for (size_t n = 0; n <= 257; ++n)
    {
        for (size_t direction = 0; direction < 2; ++direction)
        {
            for (size_t i = 0; i < n; ++i)
            {
                state = state * 1664525u + 1013904223u;
                data[i].key = (int)(state % 31); // Includes duplicate keys.
                memset(data[i].payload, data[i].key, sizeof(data[i].payload));
            }
            int order = directions[direction];
            rpcs3_ps4_qsort_r(data, n, sizeof(data[0]), compare_records, &order);
            for (size_t i = 0; i < n; ++i)
            {
                if (i) assert(compare_records(&data[i - 1], &data[i], &order) <= 0);
                for (size_t j = 0; j < sizeof(data[i].payload); ++j)
                    assert(data[i].payload[j] == (unsigned char)data[i].key);
            }
        }
    }
    rpcs3_ps4_qsort_r(NULL, 0, sizeof(data[0]), compare_records, NULL);
    rpcs3_ps4_qsort_r(NULL, 257, 0, compare_records, NULL);
    return 0;
}
