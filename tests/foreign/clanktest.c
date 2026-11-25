/* a small library for tests/test_c.fs to load and call: each kind of argument and result the c vocabulary knows */
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define EXPORTED __declspec(dllexport)
#define nap(ms) Sleep(ms)
#else
#include <time.h>
#define EXPORTED
static void nap(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}
#endif

static int flag;

EXPORTED int add2(int a, int b) { return a + b; }
EXPORTED unsigned high_bit(void) { return 0xFFFFFFF0u; }
EXPORTED long long big(void) { return 0x123456789ABCDEF0LL; }
EXPORTED long long sum8(long long a, long long b, long long c, long long d, long long e, long long f, long long g,
                        long long h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;   /* (each in its own place) */
}
EXPORTED void set_flag(int x) { flag = x; }
EXPORTED int get_flag(void) { return flag; }
EXPORTED int length_of(const char *z) { return (int)strlen(z); }
EXPORTED const char *greeting(void) { return "hello from C"; }
EXPORTED int divide(long long a, long long b, long long *quotient, long long *remainder) {
    if (b == 0) {
        return -1;
    }
    *quotient = a / b;
    *remainder = a % b;
    return 0;
}
EXPORTED void fill_bytes(unsigned char *p, int n, int value) { memset(p, value, (size_t)n); }
struct point {
    int32_t x, y;
};
EXPORTED int point_sum(const struct point *p) { return p->x + p->y; }
EXPORTED int slow_add(int a, int b) {   /* (long enough for another actor to be seen to run meanwhile) */
    nap(200);
    return a + b;
}
EXPORTED int answer = 42;   /* (a variable: c-symbol gives its address) */
