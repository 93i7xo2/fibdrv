#include "bn.h"
#include <linux/bug.h>
#include <linux/slab.h>
#include <linux/string.h>

void *bmalloc(bn_size size)
{
    void *ptr = kmalloc(size, GFP_KERNEL);
    return ptr;
}

void bfree(void *ptr)
{
    kfree(ptr);
}

static bn *bn_normalize(bn *v);
static bn *k_mul(bn *, bn *);
static bn *k_lopsided_mul(bn *, bn *);
static int kmul_split(bn *, bn_size, bn **, bn **);
static bn *x_add(bn *, bn *);
static bn *x_mul(bn *, bn *);
static digit v_iadd(digit *, bn_size, digit *, bn_size);
static digit v_isub(digit *, bn_size, digit *, bn_size);

bn *bn_new(bn_size size)
{
    bn *ret =
        (bn *) bmalloc(__builtin_offsetof(bn, bn_digit) + sizeof(digit) * size);
    if (!ret)
        return NULL;
    ret->size = size;
    ret->capacity = size;
    Bn_SETREF(ret, 1);
    return ret;
}


bn *bn_new_from_digit(digit i)
{
    bn *ret = bn_new(sizeof(digit));
    if (!ret)
        return NULL;
    ret->bn_digit[0] = (digit)(i & Bn_MASK);
    Bn_SET_SIZE(ret, 1);
    return ret;
}

bn *bn_new_from_twodigits(twodigits i)
{
    bn *ret = bn_new(sizeof(twodigits));
    if (!ret)
        return NULL;
    ret->bn_digit[0] = (digit)(i & Bn_MASK);
    ret->bn_digit[1] = (digit)((i >> Bn_SHIFT) & Bn_MASK);
    Bn_SET_SIZE(ret, 2);
    return bn_normalize(ret);
}


bn *bn_mul(bn *a, bn *b)
{
    if (Bn_SIZE(a) <= 1 && Bn_SIZE(b) <= 1) {
        twodigits s = ((twodigits) a->bn_digit[0]) * b->bn_digit[0];
        return bn_new_from_twodigits(s);
    }

    bn *z = k_mul(a, b);
    /* Negate if exactly one of the inputs is negative. */
    if (z && ((Bn_SIZE(a) ^ Bn_SIZE(b)) < 0)) {
        Bn_SET_SIZE(z, -z->size);
    }
    return z;
}

bn *bn_add(bn *a, bn *b)
{
    return x_add(a, b);
}

static bn *bn_normalize(bn *v)
{
    bn_size j = Bn_ABS(Bn_SIZE(v));
    bn_size i = j;
    while (i > 0 && v->bn_digit[i - 1] == 0)
        --i;
    if (i != j) {
        Bn_SET_SIZE(v, (Bn_SIZE(v) < 0) ? -(i) : i);
    }
    return v;
}


/* Karatsuba multiplication. Ignores the input signs,
 * and returns the absolute value of the product. */
static bn *k_mul(bn *a, bn *b)
{
    bn_size size_a = Bn_ABS(Bn_SIZE(a)), size_b = Bn_ABS(Bn_SIZE(b));
    bn *ah, *al, *bh, *bl, *ret;
    ah = al = bh = bl = ret = NULL;

    /* Recall:
     *     a = ah*B^m + al
     *     b = bh*B^m + bl
     * Then
     *     a * b = (ah*bh)*B^2m + (ah*bl+al*bh)*B^m + (al*bl)
     * where
     *     B = 2
     *     m = shift
     */

    /* Make sure b is the largest number. */
    if (size_a > size_b) {
        bn *tmp = a;
        a = b;
        b = tmp;

        bn_size size_tmp = size_a;
        size_a = size_b;
        size_b = size_tmp;
    }

    /* Use grade-school multiplication when either number is too small */
    bn_size i = a == b ? KARATSUBA_SQUARE_CUTOFF : KARATSUBA_CUTOFF;
    if (size_a <= i) {
        if (size_a == 0)
            return bn_new_from_digit(0);
        else
            return x_mul(a, b);
    }

    /* If a is small compared to b, splitting on b gives a degenerate
     * case with ah==0, and Karatsuba may be (even much) less efficient
     * than "grade school" then.  However, we can still win, by viewing
     * b as a string of "big digits", each of width a->ob_size.  That
     * leads to a sequence of balanced calls to k_mul.
     */
    if (2 * size_a <= size_b)
        return k_lopsided_mul(a, b);

    /* Split a & b into hi & lo pieces. */
    bn_size shift = size_b >> 1;
    if (kmul_split(a, shift, &ah, &al) < 0)
        goto fail;
    BUG_ON(Bn_SIZE(ah) <= 0); /* the split isn't degenerate */

    if (a == b) {
        bh = ah;
        bl = al;
        Bn_INCREF(bh);
        Bn_INCREF(bl);
    } else if (kmul_split(b, shift, &bh, &bl) < 0)
        goto fail;

    /* Recall:
     *     a * b = (ah*bh)*B^2m + (ah*bl+al*bh)*B^m + (al*bl) = result
     * and
     *    (ah*bl+al*bh) can be written as (ah+al)(bh+bl) - ah*bh - al*bl
     *
     * To do:
     * 1. Allocate space
     * 2. t1 <- ah*bh, result += t1*B^2m
     * 3. t2 <- al*bl, result += t2
     * 4. result -= t2
     * 5. result -= t1
     * 6. t3 <- (ah+al)(bh+bl), result += t3
     */

    /* 1. Allocate space */
    ret = bn_new(size_a + size_b);
    if (!ret)
        goto fail;

    /* 2. t1 <- ah*bh, and copy into high digits of result. */
    bn *t1, *t2, *t3;
    if (!(t1 = k_mul(ah, bh)))
        goto fail;
    BUG_ON(Bn_SIZE(t1) < 0);
    BUG_ON(2 * shift + Bn_SIZE(t1) > Bn_SIZE(ret));
    memcpy(ret->bn_digit + 2 * shift, t1->bn_digit,
           Bn_SIZE(t1) * sizeof(digit));

    /* Zero-out the digits higher than the ah*bh copy. */
    i = Bn_SIZE(ret) - 2 * shift - Bn_SIZE(t1);
    if (i)
        memset(ret->bn_digit + 2 * shift + Bn_SIZE(t1), 0, i * sizeof(digit));

    /* 3. t2 <- al*bl, and copy into the low digits. */
    if ((t2 = k_mul(al, bl)) == NULL) {
        Bn_DECREF(t1);
        goto fail;
    }
    BUG_ON(Bn_SIZE(t2) < 0);
    BUG_ON(Bn_SIZE(t2) > 2 * shift);
    memcpy(ret->bn_digit, t2->bn_digit, Bn_SIZE(t2) * sizeof(digit));

    /* Zero out remaining digits. */
    i = 2 * shift - Bn_SIZE(t2);
    if (i)
        memset(ret->bn_digit + Bn_SIZE(t2), 0, i * sizeof(digit));

    /* 4. & 5. Substract al*bl and ah*bh from result. We do al*bl first
     * because it's fresher in cache.
     */
    i = Bn_SIZE(ret) - shift; /* # digits after shift */
    v_isub(ret->bn_digit + shift, i, t2->bn_digit, Bn_SIZE(t2));
    Bn_DECREF(t2);

    v_isub(ret->bn_digit + shift, i, t1->bn_digit, Bn_SIZE(t1));
    Bn_DECREF(t1);

    /* 6. t3 <- (ah+al)(bh+bl), result += t3 */
    if ((t1 = x_add(ah, al)) == NULL)
        goto fail;
    Bn_DECREF(ah);
    Bn_DECREF(al);
    ah = al = NULL;

    if (a == b) {
        t2 = t1;
        Bn_INCREF(t2);
    } else if ((t2 = x_add(bh, bl)) == NULL) {
        Bn_DECREF(t1);
        goto fail;
    }
    Bn_DECREF(bh);
    Bn_DECREF(bl);
    bh = bl = NULL;

    t3 = k_mul(t1, t2);
    Bn_DECREF(t1);
    Bn_DECREF(t2);
    if (t3 == NULL)
        goto fail;
    BUG_ON(Bn_SIZE(t3) < 0);

    /* Add t3.  It's not obvious why we can't run out of room here.
     * See the (*) comment after this function.
     */
    v_iadd(ret->bn_digit + shift, i, t3->bn_digit, Bn_SIZE(t3));
    Bn_DECREF(t3);

    return bn_normalize(ret);

fail:
    Bn_DECREF(ret);
    Bn_DECREF(ah);
    Bn_DECREF(al);
    Bn_DECREF(bh);
    Bn_DECREF(bl);
    return NULL;
}

static bn *k_lopsided_mul(bn *a, bn *b)
{
    const bn_size asize = Bn_ABS(Bn_SIZE(a));
    bn_size bsize = Bn_ABS(Bn_SIZE(b));
    bn_size nbdone; /* # of b digits already multiplied */
    bn *ret;
    bn *bslice = NULL;

    BUG_ON(asize <= KARATSUBA_CUTOFF);
    BUG_ON(2 * asize > bsize);

    /* Allocate result space, and zero it out. */
    ret = bn_new(asize + bsize);
    if (ret == NULL)
        return NULL;
    memset(ret->bn_digit, 0, Bn_SIZE(ret) * sizeof(digit));

    /* Successive slices of b are copied into bslice. */
    bslice = bn_new(asize);
    if (bslice == NULL)
        goto fail;

    nbdone = 0;
    while (bsize > 0) {
        bn *product;
        const bn_size nbtouse = Bn_MIN(bsize, asize);

        /* Multiply the next slice of b by a. */
        memcpy(bslice->bn_digit, b->bn_digit + nbdone, nbtouse * sizeof(digit));
        Bn_SET_SIZE(bslice, nbtouse);
        product = k_mul(a, bslice);
        if (product == NULL)
            goto fail;

        /* Add into result. */
        v_iadd(ret->bn_digit + nbdone, Bn_SIZE(ret) - nbdone, product->bn_digit,
               Bn_SIZE(product));
        Bn_DECREF(product);

        bsize -= nbtouse;
        nbdone += nbtouse;
    }

    Bn_DECREF(bslice);
    return bn_normalize(ret);

fail:
    Bn_DECREF(ret);
    Bn_DECREF(bslice);
    return NULL;
}


static int kmul_split(bn *n, bn_size size, bn **high, bn **low)
{
    bn *hi, *lo;
    bn_size size_lo, size_hi;
    const bn_size size_n = Bn_ABS(Bn_SIZE(n));

    size_lo = Bn_MIN(size_n, size);
    size_hi = size_n - size_lo;

    if ((hi = bn_new(size_hi)) == NULL)
        return -1;
    if ((lo = bn_new(size_lo)) == NULL) {
        Bn_DECREF(hi);
        return -1;
    }

    memcpy(lo->bn_digit, n->bn_digit, size_lo * sizeof(digit));
    memcpy(hi->bn_digit, n->bn_digit + size_lo, size_hi * sizeof(digit));

    *high = bn_normalize(hi);
    *low = bn_normalize(lo);
    return 0;
}

static bn *x_add(bn *a, bn *b)
{
    bn_size size_a = Bn_ABS(Bn_SIZE(a)), size_b = Bn_ABS(Bn_SIZE(b));
    digit carry = 0;

    /* Ensure a is larger than b */
    if (size_a < size_b) {
        bn *tmp = a;
        a = b;
        b = tmp;

        bn_size size_tmp = size_a;
        size_a = size_b;
        size_b = size_tmp;
    }

    bn *z = bn_new(size_a + 1);
    if (!z)
        return NULL;

    bn_size i;
    for (i = 0; i < size_b; ++i) {
        carry += a->bn_digit[i] + b->bn_digit[i];
        z->bn_digit[i] = carry & Bn_MASK;
        carry >>= Bn_SHIFT;
    }
    for (; i < size_a; ++i) {
        carry += a->bn_digit[i];
        z->bn_digit[i] = carry & Bn_MASK;
        carry >>= Bn_SHIFT;
    }
    z->bn_digit[i] = carry;
    return bn_normalize(z);
}


/* x[0:m] and y[0:n] are digit vectors, LSD first, m >= n required.  x[0:n]
 * is modified in place, by adding y to it.  Carries are propagated as far as
 * x[m-1], and the remaining carry (0 or 1) is returned.
 */

static digit v_iadd(digit *x, bn_size m, digit *y, bn_size n)
{
    bn_size i;
    digit carry = 0;

    BUG_ON(m < n);
    for (i = 0; i < n; ++i) {
        carry += x[i] + y[i];
        x[i] = carry & Bn_MASK;
        carry >>= Bn_SHIFT;
        BUG_ON((carry & 1) != carry);
    }
    for (; carry && i < m; ++i) {
        carry += x[i];
        x[i] = carry & Bn_MASK;
        carry >>= Bn_SHIFT;
        BUG_ON((carry & 1) != carry);
    }
    return carry;
}

/* x[0:m] and y[0:n] are digit vectors, LSD first, m >= n required.  x[0:n]
 * is modified in place, by subtracting y from it.  Borrows are propagated as
 * far as x[m-1], and the remaining borrow (0 or 1) is returned.
 */
static digit v_isub(digit *x, bn_size m, digit *y, bn_size n)
{
    bn_size i;
    digit borrow = 0;

    BUG_ON(m < n);
    for (i = 0; i < n; ++i) {
        borrow = x[i] - y[i] - borrow;
        x[i] = borrow & Bn_MASK;
        borrow >>= Bn_SHIFT;
        borrow &= 1; /* keep only 1 sign bit */
    }
    for (; borrow && i < m; ++i) {
    }
    return borrow;
}


/* Grade school multiplication, ignoring the signs.
 * Returns the absolute value of the product, or NULL if error.
 */

static bn *x_mul(bn *a, bn *b)
{
    bn *z;
    bn_size size_a = Bn_ABS(Bn_SIZE(a));
    bn_size size_b = Bn_ABS(Bn_SIZE(b));
    bn_size i;

    z = bn_new(size_a + size_b);
    if (z == NULL)
        return NULL;

    memset(z->bn_digit, 0, Bn_SIZE(z) * sizeof(digit));
    if (a == b) {
        /* Efficient squaring per HAC, Algorithm 14.16:
         * http://www.cacr.math.uwaterloo.ca/hac/about/chap14.pdf
         * Gives slightly less than a 2x speedup when a == b,
         * via exploiting that each entry in the multiplication
         * pyramid appears twice (except for the size_a squares).
         */
        for (i = 0; i < size_a; ++i) {
            twodigits carry;
            twodigits f = a->bn_digit[i];
            digit *pz = z->bn_digit + (i << 1);
            digit *pa = a->bn_digit + i + 1;
            digit *paend = a->bn_digit + size_a;

            carry = *pz + f * f;
            *pz++ = (digit)(carry & Bn_MASK);
            carry >>= Bn_SHIFT;
            BUG_ON(carry > Bn_MASK);

            /* Now f is added in twice in each column of the
             * pyramid it appears.  Same as adding f<<1 once.
             */
            f <<= 1;
            while (pa < paend) {
                carry += *pz + *pa++ * f;
                *pz++ = (digit)(carry & Bn_MASK);
                carry >>= Bn_SHIFT;
                BUG_ON(carry > (Bn_MASK << 1));
            }
            if (carry) {
                carry += *pz;
                *pz++ = (digit)(carry & Bn_MASK);
                carry >>= Bn_SHIFT;
            }
            if (carry)
                *pz += (digit)(carry & Bn_MASK);
            BUG_ON((carry >> Bn_SHIFT) != 0);
        }
    } else {
        for (i = 0; i < size_a; ++i) {
            twodigits carry = 0;
            twodigits f = a->bn_digit[i];
            digit *pz = z->bn_digit + i;
            digit *pb = b->bn_digit;
            digit *pbend = b->bn_digit + size_b;

            while (pb < pbend) {
                carry += *pz + *pb++ * f;
                *pz++ = (digit)(carry & Bn_MASK);
                carry >>= Bn_SHIFT;
                BUG_ON(carry > Bn_MASK);
            }
            if (carry)
                *pz += (digit)(carry & Bn_MASK);

            BUG_ON((carry >> Bn_SHIFT) != 0);
        }
    }
    return bn_normalize(z);
}

bn *bn_to_dec(bn *a)
{
    /* The maximum number stored in 'a' is 2^(maxbits)-1 ,so we
     * need (maxbits * log(2) / log(10) + 1) digits to present
     * each decimal number.
     */
    bn_size maxbits = Bn_ABS(Bn_SIZE(a)) * ((sizeof(digit) << 3) - 1);

    /* 0.3010 = 2^9 / 1701
     * maxbits = p*1701 + q, q<1701
     */
    bn_size p = maxbits / 1701, q = maxbits % 1701;
    bn_size new_size = (p << 9) + (q << 9) / 1701;
    new_size += 1;

    bn *str = bn_new(new_size);
    memset(str->bn_digit, 0, sizeof(digit) * Bn_ABS(Bn_SIZE(str)));

    bn_size i, z = 0;
    twodigits carry;
    while (z < Bn_SIZE(str)) {
        for (i = Bn_SIZE(a), carry = 0; i > 0; --i) {
            carry = carry << Bn_SHIFT | a->bn_digit[i - 1];
            a->bn_digit[i - 1] = (digit)(carry / 10);
            carry %= 10;
        }
        str->bn_digit[z++] = carry;
    }
    return bn_normalize(str);
}

char *bn_to_str(bn *dec)
{
    bn_size i = 0, n = Bn_ABS(Bn_SIZE(dec));
    char *str;
    if (!n) {
        str = (char *) bmalloc(sizeof(char) * 2);
        str[0] = '0';
        str[1] = '\0';
        return str;
    }
    str = (char *) bmalloc(sizeof(char) * (n + 1));
    while (n > 0) {
        str[i++] = (dec->bn_digit[(n--) - 1] & Bn_MASK) | 0x30;
    }
    str[i] = 0;
    return str;
}
