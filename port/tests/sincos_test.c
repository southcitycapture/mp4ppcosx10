/* M42 (PLAN.md 57.3): port_fast_sincosf against libm's sinf/cosf for EVERY
 * float of its domain (2^-26 <= |x| <= 1024, both signs, and the zeros), on
 * two threads.
 * Prints the differences (must be 0), the guard's refusals, and the time a
 * pair takes each way.   sincos_test [stride=1] */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "port.h"
int port_fast_sincosf(float x, float* s, float* c);
PortOptions port_opt;
void port_log(const char* fmt, ...) { (void)fmt; }

static unsigned stride = 1;
typedef struct { unsigned lo, hi; unsigned long n, diff, refused; } Job;

static float f_of(unsigned u) { float f; memcpy(&f, &u, 4); return f; }
static unsigned u_of(float f) { unsigned u; memcpy(&u, &f, 4); return u; }

static void* run(void* a) {
    Job* j = a;
    unsigned u;
    for (u = j->lo; u <= j->hi && u >= j->lo; u += stride) {
        int sg;
        for (sg = 0; sg < 2; sg++) {
            float x = f_of(u | (sg ? 0x80000000u : 0)), s, c;
            j->n++;
            if (!port_fast_sincosf(x, &s, &c)) { j->refused++; continue; }
            if (u_of(s) != u_of(sinf(x)) || u_of(c) != u_of(cosf(x))) {
                if (j->diff < 20)
                    printf("DIFF x=%a (%.9g): fast %a %a libm %a %a\n", x, x, s, c, sinf(x), cosf(x));
                j->diff++;
            }
        }
        if (u > 0xFFFFFFFFu - stride) break;
    }
    return NULL;
}

static double now(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec * 1e-6; }

int main(int argc, char** argv) {
    unsigned lo = 0x32800000u, hi = 0x44800000u, mid; /* 2^-26 .. 1024 */
    pthread_t t[2];
    Job j[2];
    double t0;
    if (argc > 1) stride = (unsigned)atoi(argv[1]);
    mid = lo + (hi - lo) / 2;
    memset(j, 0, sizeof(j));
    j[0].lo = lo; j[0].hi = mid - 1; j[1].lo = mid; j[1].hi = hi;
    t0 = now();
    pthread_create(&t[0], NULL, run, &j[0]);
    pthread_create(&t[1], NULL, run, &j[1]);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    {   /* the two zeros */
        float z[2] = { 0.0f, -0.0f }, s, c;
        int i;
        for (i = 0; i < 2; i++) {
            j[0].n++;
            if (!port_fast_sincosf(z[i], &s, &c)) { j[0].refused++; continue; }
            if (u_of(s) != u_of(sinf(z[i])) || u_of(c) != u_of(cosf(z[i]))) { j[0].diff++; printf("DIFF at a zero\n"); }
        }
    }
    printf("sincos_test: %lu floats (stride %u) of 2^-26 <= |x| <= 1024 and the zeros: %lu differ, %lu refused by the guard (libm), %.1f s\n",
           j[0].n + j[1].n, stride, j[0].diff + j[1].diff, j[0].refused + j[1].refused, now() - t0);
    {   /* the cost of a pair each way, one thread, 2M angles over the game's range */
        volatile float vs, vc;
        (void)vs; (void)vc;
        float s, c;
        int i, k = 2000000;
        double a, b;
        t0 = now();
        for (i = 0; i < k; i++) { float x = -7.0f + 14.0f * (float)i / (float)k; vs = sinf(x); vc = cosf(x); }
        a = now() - t0;
        t0 = now();
        for (i = 0; i < k; i++) { float x = -7.0f + 14.0f * (float)i / (float)k;
            if (!port_fast_sincosf(x, &s, &c)) { s = sinf(x); c = cosf(x); } vs = s; vc = c; }
        b = now() - t0;
        printf("sincos_test: a pair, libm %.0f ns, fast %.0f ns\n", a / k * 1e9, b / k * 1e9);
    }
    return (j[0].diff + j[1].diff) ? 1 : 0;
}
