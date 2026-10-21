#include "brain/brain.h"

/* Stateless splitmix finalizer. No shared PRNG state, no visit-order dependence. */
uint64_t brain_random_key(uint64_t seed, uint64_t domain, uint64_t object_id) {
    uint64_t x = seed ^ (domain * UINT64_C(0x9e3779b97f4a7c15)) ^
                 (object_id * UINT64_C(0xbf58476d1ce4e5b9));
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
