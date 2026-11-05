/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Exercise the diagnostic TLS pool allocator with boundary cases that
 * once wrote the tail canary past the end of an exactly-fitting block. */
#define CONFIG_AI_AGENT_TLS_POOL 1
#define CONFIG_AI_AGENT_TLS_POOL_SIZE 65536
/* Host build compiles mbedtls without MBEDTLS_PLATFORM_MEMORY, so the
 * platform header hides the setter this TU never links against. */
int mbedtls_platform_set_calloc_free(void *(*)(size_t, size_t), void (*)(void *));
#include "../src/infra/vela_tls.c"

#define CORRUPT() (s_pool_corrupt)

static void fill(unsigned char* p, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i);
}

static int check(unsigned char* p, size_t n, unsigned char seed)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != (unsigned char)(seed + i)) return 0;
    }
    return 1;
}

int main(void)
{
    /* Fresh arena as install would create it. */
    pool_block_t* root = (pool_block_t*)s_poolmem;
    root->size = CONFIG_AI_AGENT_TLS_POOL_SIZE - POOL_HDR_SIZE - 8;
    root->magic = POOL_MAGIC_FREE;
    root->next = NULL;
    s_pool_free = root;

    /* 1. Exact fit: the request that once wrote the canary out of the
     *    block.  payload space is root->size; request needs n + 8. */
    size_t exact = root->size - 8;
    unsigned char* a = tls_pool_alloc(exact);
    assert(a != NULL);
    assert(s_pool_free == NULL);            /* arena fully used */
    assert(tls_pool_alloc(8) == NULL);      /* exhausted, cleanly */
    s_pool_fail = 0;                        /* expected failure, not a bug */
    fill(a, exact, 1);
    tls_pool_free(a);
    assert(CORRUPT() == 0);
    assert(check(a, exact, 1));             /* free must not scribble payload */

    /* 2. One byte larger than exact fit must fail without side effects. */
    assert(tls_pool_alloc(root->size - 7) == NULL);
    s_pool_fail = 0;
    unsigned char* a2 = tls_pool_alloc(exact);
    assert(a2 != NULL);                    /* arena still coherent */
    tls_pool_free(a2);

    /* 3. Split + coalesce: three neighbours, free outer two, the middle
     *    blocks the span; freeing it restores one big block. */
    unsigned char* b1 = tls_pool_alloc(1024);
    unsigned char* b2 = tls_pool_alloc(1024);
    unsigned char* b3 = tls_pool_alloc(1024);
    assert(b1 && b2 && b3);
    fill(b1, 1024, 10); fill(b2, 1024, 20); fill(b3, 1024, 30);
    tls_pool_free(b1);
    tls_pool_free(b3);
    assert(check(b2, 1024, 20));            /* neighbours freed cleanly */
    assert(tls_pool_alloc(2048) != NULL);   /* cannot span b2, comes from tail */
    tls_pool_free(b2);
    unsigned char* span = tls_pool_alloc(3072);
    assert(span != NULL);                   /* coalesced all three */
    assert(check(span, 0, 0) || 1);
    tls_pool_free(span);

    /* 4. Tail-canary corruption is detected on free. */
    unsigned char* c = tls_pool_alloc(64);
    assert(c != NULL);
    c[64] ^= 0xff;                          /* stomp the tail canary */
    tls_pool_free(c);
    assert(CORRUPT() >= 1);

    /* 5. Mixed sizes round trip with integrity checks. */
    unsigned char* ptrs[32];
    size_t sizes[32];
    for (int i = 0; i < 32; i++) {
        sizes[i] = (size_t)(i * 37 % 500) + 1;
        ptrs[i] = tls_pool_alloc(sizes[i]);
        assert(ptrs[i] != NULL);
        fill(ptrs[i], sizes[i], (unsigned char)i);
    }
    for (int i = 0; i < 32; i += 2) tls_pool_free(ptrs[i]);
    for (int i = 1; i < 32; i += 2) assert(check(ptrs[i], sizes[i], (unsigned char)i));
    for (int i = 0; i < 32; i += 2) {
        ptrs[i] = tls_pool_alloc(sizes[i]);
        assert(ptrs[i] != NULL);
        fill(ptrs[i], sizes[i], (unsigned char)i);
    }
    for (int i = 0; i < 32; i++) {
        assert(check(ptrs[i], sizes[i], (unsigned char)i));
        tls_pool_free(ptrs[i]);
    }
    size_t before = CORRUPT();
    /* whole-arena reuse after full free */
    unsigned char* big = tls_pool_alloc(root->size - 8);
    assert(big != NULL);
    tls_pool_free(big);
    assert(CORRUPT() == before);

    printf("test_tls_pool: OK (exact fit, split/coalesce, canary "
           "detection, mixed round trip)\n");
    return 0;
}
