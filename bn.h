#ifndef __BN__
#define __BN__

#include <linux/types.h>

typedef long long int bn_size;
typedef unsigned short digit;
typedef unsigned int twodigits;

#define Bn_SHIFT ((sizeof(digit) << 3) - 1)
#define Bn_MASK ((digit)((1 << Bn_SHIFT) - 1))

typedef struct {
    bn_size size;
    bn_size capacity;
    size_t refcnt;
    digit bn_digit[0];
} bn;

void *bmalloc(bn_size);
void bfree(void *);

#define Bn_MIN(x, y) ((x) < (y) ? (x) : (y))
#define Bn_SIZE(x) ((x)->size)
#define Bn_ABS(x)                                \
    (((x >> ((sizeof(bn_size) << 3) - 1)) ^ x) - \
     (x >> ((sizeof(bn_size) << 3) - 1)))
#define Bn_INCREF(x) ((x)->refcnt++)
#define Bn_SETREF(x, i) ((x)->refcnt = i)
#define Bn_DECREF(x)                 \
    do {                             \
        if (x && (--x->refcnt == 0)) \
            bfree(x);                \
    } while (0)
#define Bn_SET_SIZE(x, c) ((x)->size = c)

#define KARATSUBA_CUTOFF 70
#define KARATSUBA_SQUARE_CUTOFF KARATSUBA_CUTOFF << 1

#define _swap(x, y) \
    do {            \
        x = x ^ y;  \
        y = x ^ y;  \
        x = x ^ y;  \
    } while (0)

bn *bn_new(bn_size);
bn *bn_new_from_digit(digit);
bn *bn_new_from_twodigits(twodigits);
bn *bn_mul(bn *a, bn *b);
bn *bn_add(bn *, bn *);
bn *bn_to_dec(bn *);
char *bn_to_str(bn *);

#endif