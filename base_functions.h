/*
base_functions.h : A lot of base generic functions and minor structures used
by many portions of the lsvd system:
	-debug initializations
	-_log structure
	-rounding up functions
	-wrapper functions
*/

#ifndef BASE_FUNCTIONS_H
#define BASE_FUNCTIONS_H

#include <uuid/uuid.h>
#include <sys/uio.h>

#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>

enum {DBG_MAP = 1, DBG_HITS = 2, DBG_AIO = 4};

struct _log {
    int l;
    pthread_t th;
    long arg;
} *logbuf, *logptr;

void dbg(int l, long arg)
{
    logptr->l = l;
    logptr->th = pthread_self();
    logptr->arg = arg;
    logptr++;
}
//#define DBG(a) dbg(__LINE__, a)
#define DBG(a) 

// https://stackoverflow.com/questions/5008804/generating-random-integer-from-a-range
std::random_device rd;     // only used once to initialise (seed) engine
//std::mt19937 rng(rd());  // random-number engine used (Mersenne-Twister in this case)
std::mt19937 rng(17);      // for deterministic testing

typedef int64_t sector_t;
typedef int page_t;

/* make this atomic? */
int batch_seq;
int last_ckpt;
const int BATCH_SIZE = 8 * 1024 * 1024;
uuid_t my_uuid;

// div_round_up :	This function simply take two numbers and divides them rounding up.
int div_round_up(int n, int m)
{
    return (n + m - 1) / m;
}

// round_up :	This function rounds up a number n to the nearest multiple of m
int round_up(int n, int m)
{
    return m * div_round_up(n, m);
}

// iov_sum :	Takes the sum of lengths of each element in iov
size_t iov_sum(const iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
	sum += iov[i].iov_len;
    return sum;
}

// hex :	Converts uint32_t number to hex
std::string hex(uint32_t n)
{
    std::stringstream stream;
    stream << std::setfill ('0') << std::setw(8) << std::hex << n;
    return stream.str();
}

/* ------- */

/* simple hack so we can pass lambdas through C callback mechanisms
 */
struct wrapper {
    std::function<bool()> f;
    wrapper(std::function<bool()> _f) : f(_f) {}
};

/* invoked function returns boolean - if true, delete the wrapper; otherwise
 * keep it around for another invocation
 */
void *wrap(std::function<bool()> _f)
{
    auto s = new wrapper(_f);
    return (void*)s;
}

void call_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    if (std::invoke(s->f))
	delete s;
}

void delete_wrapped(void *ptr)
{
    auto s = (wrapper*)ptr;
    delete s;
}

/* ----- */

#endif
