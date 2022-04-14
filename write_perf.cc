/*
 * test performance of various I/O methods with fio
 * link this with fio instead of librbd
 */
#include <rados/librados.h>
#include <rbd/librbd.h>

#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>
#include <cassert>
#include <random>

#include <fcntl.h>
#include <string.h>
#include <aio.h>

#if 0
#include <ios>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <list>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <future>
#endif

std::mt19937 rng(17);      // for deterministic testing

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

/* helper functions */

static std::pair<std::string,std::string> split_string(std::string s, std::string delim)
{
    auto i = s.find(delim);
    return std::pair(s.substr(0,i), s.substr(i+delim.length()));
}

size_t iov_sum(const iovec *iov, int iovcnt)
{
    size_t sum = 0;
    for (int i = 0; i < iovcnt; i++)
        sum += iov[i].iov_len;
    return sum;
}

static bool aligned(int a, const void *ptr)
{
    return 0 == ((long)ptr & (a-1));
}

typedef void (*posix_aio_cb_t)(union sigval siv);
void aio_wrapper_done(union sigval siv);

struct aio_wrapper {
    aiocb aio = {0};
    void (*cb)(void*);
    void *ptr = NULL;
    aio_wrapper(int fd, char *buf, size_t nbytes, off_t offset,
		   void (*_cb)(void*), void *_ptr) {
	cb = _cb;
	ptr = _ptr;
	aio.aio_fildes = fd;
	aio.aio_buf = buf;
	aio.aio_offset = offset;
	aio.aio_nbytes = nbytes;
	aio.aio_sigevent.sigev_notify = SIGEV_THREAD;
	aio.aio_sigevent.sigev_value.sival_ptr = (void*)this;
	aio.aio_sigevent.sigev_notify_function = aio_wrapper_done;
    }
};

void aio_wrapper_done(union sigval siv)
{
    auto w = (aio_wrapper*)siv.sival_ptr;
    int err = aio_error(&w->aio);
    assert(err != EINPROGRESS);
    int rv = aio_return(&w->aio);
    //printf("%p fd %d op=%s rv=%d err=%d\n", w, w->aio.aio_fildes, aio_op(w->aio.aio_lio_opcode), rv, err);
    w->cb(w->ptr);
    delete w;
}

/* ------------------- FAKE RBD INTERFACE ----------------------*/

struct rbd_ops {
    int (*aio_readv)(rbd_image_t image, const iovec *iov, int iovcnt, uint64_t off, rbd_completion_t c);
    int (*aio_writev)(rbd_image_t image, const struct iovec *iov, int iovcnt, uint64_t off, rbd_completion_t c);
    int (*aio_read)(rbd_image_t image, uint64_t offset, size_t len, char *buf, rbd_completion_t c);
    int (*aio_write)(rbd_image_t image, uint64_t off, size_t len, const char *buf, rbd_completion_t c);
};

/* now our fake implementation
 */
struct fake_rbd_image {
    std::mutex   m;
    int          fd;		// cache device
    ssize_t      vol_size;	// bytes
    ssize_t      dev_size;
    rbd_ops     *ops;
    std::queue<std::thread> threads;
    bool         closed = false;
    std::queue<aiocb*> aios;
    std::condition_variable cv;
};

struct lsvd_completion {
public:
    fake_rbd_image *fri = NULL;
    rbd_callback_t cb = NULL;
    void *arg = NULL;
    int retval = 0;
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<int> refcount = 1;
    std::atomic<int> n = 0;

    lsvd_completion(rbd_callback_t _cb, void *_arg) : cb(_cb), arg(_arg) {}
};

/* eventually I'll figure out how to do eventfd notification
 */
extern "C" int rbd_poll_io_events(rbd_image_t image, rbd_completion_t *comps, int numcomp)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return 0;
}

extern "C" int rbd_set_image_notification(rbd_image_t image, int fd, int type)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return 0;
}

extern "C" int rbd_aio_create_completion(void *cb_arg,
					 rbd_callback_t complete_cb, rbd_completion_t *c)
{
    lsvd_completion *p = new lsvd_completion(complete_cb, cb_arg);
    *c = (rbd_completion_t)p;
    return 0;
}

extern "C" void rbd_aio_release(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    if (--p->refcount)
	delete p;
}

extern "C" int rbd_aio_discard(rbd_image_t image, uint64_t off, uint64_t len, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;
    p->cb(c, p->arg);
    return 0;
}

extern "C" int rbd_aio_flush(rbd_image_t image, rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    p->fri = (fake_rbd_image*)image;
    p->cb(c, p->arg);
    return 0;
}

extern "C" int rbd_flush(rbd_image_t image)
{
    auto fri = (fake_rbd_image*)image;
    return 0;
}

extern "C" void *rbd_aio_get_arg(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->arg;
}

extern "C" ssize_t rbd_aio_get_return_value(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    return p->retval;
}
    
extern "C" int rbd_aio_read(rbd_image_t image, uint64_t offset, size_t len, char *buf,
			    rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return fri->ops->aio_read(image, offset, len, buf, c);
}

static int emulate_aio_read(rbd_image_t image, uint64_t offset, size_t len, char *buf,
			    rbd_completion_t c)
{
    iovec iov = {(void*)buf, len};
    return rbd_aio_readv(image, &iov, 1, offset, c);
}
 
extern "C" int rbd_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			     rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return fri->ops->aio_write(image, off, len, buf, c);

}

static int emulate_aio_write(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			     rbd_completion_t c)
{
    iovec iov = {(void*)buf, len};
    return rbd_aio_writev(image, &iov, 1, off, c);
}

/* TODO - add optional buffer to lsvd_completion, 
 *   completion copies (for read) and frees 
 */
extern "C" int rbd_aio_readv(rbd_image_t image, const iovec *iov,
			     int iovcnt, uint64_t off, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return fri->ops->aio_readv(image, iov, iovcnt, off, c);
}

extern "C" int rbd_aio_writev(rbd_image_t image, const struct iovec *iov,
			      int iovcnt, uint64_t off, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    return fri->ops->aio_writev(image, iov, iovcnt, off, c);
}

void rbd_call_wrapped(rbd_completion_t c, void *ptr)
{
    call_wrapped(ptr);
}

/* note that rbd_aio_read handles aligned bounce buffers for us
 */
extern "C" ssize_t rbd_read(rbd_image_t image, uint64_t off, size_t len, char *buf)
{
    rbd_completion_t c;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    void *closure = wrap([&m, &cv, &done]{
	    done = true;
	    cv.notify_all();
	    return true;
	});
    rbd_aio_create_completion(closure, rbd_call_wrapped, &c);

    std::unique_lock lk(m);
    rbd_aio_read(image, off, len, buf, c);
    while (!done)
	cv.wait(lk);
    auto val = rbd_aio_get_return_value(c);
    rbd_aio_release(c);
    return val;
}

extern "C" ssize_t rbd_write(rbd_image_t image, uint64_t off, size_t len, const char *buf)
{
    rbd_completion_t c;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    void *closure = wrap([&m, &cv, &done]{
	    std::unique_lock lk(m);
	    done = true;
	    cv.notify_all();
	    return true;
	});
    rbd_aio_create_completion(closure, rbd_call_wrapped, &c);

    std::unique_lock lk(m);
    rbd_aio_write(image, off, len, buf, c);
    while (!done)
	cv.wait(lk);
    auto val = rbd_aio_get_return_value(c);
    rbd_aio_release(c);
    return val;
}

extern "C" int rbd_aio_wait_for_complete(rbd_completion_t c)
{
    lsvd_completion *p = (lsvd_completion *)c;
    std::unique_lock lk(p->m);
    p->refcount++;
    while (!p->done)
	p->cv.wait(lk);
    if (--p->refcount == 0)
	delete p;
    return 0;
}

extern "C" int rbd_stat(rbd_image_t image, rbd_image_info_t *info, size_t infosize)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    info->size = fri->vol_size;
    return 0;
}

extern "C" int rbd_get_size(rbd_image_t image, uint64_t *size)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    *size = fri->vol_size;
    return 0;
}

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

ssize_t getsize64(int fd)
{
    struct stat sb;
    size_t size;
    
    if (fstat(fd, &sb) < 0)
	return -1;
    if (S_ISBLK(sb.st_mode)) {
	if (ioctl(fd, BLKGETSIZE64, &size) < 0)
	    return -1;
    }
    else
	size = sb.st_size;
    return size;
}

/* implementation 1 */

static int rbd_aio_writev_1(rbd_image_t image, const struct iovec *iov,
			    int iovcnt, uint64_t off, rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto p = (lsvd_completion *)c;
    auto len = iov_sum(iov, iovcnt);

    assert(aligned(512, iov[0].iov_base));
    int n = fri->dev_size / len;
    std::uniform_int_distribution<int> uni(0, n - 1);
    int j = uni(rng);

    if (pwritev(fri->fd, iov, iovcnt, j*len) < 0) {
	perror("dev pwrite");
	return -1;
    }
    p->cb(c, p->arg);
    return 0;
}

static rbd_ops ops_1 = {
    .aio_readv = NULL,
    .aio_writev = rbd_aio_writev_1,
    .aio_read = emulate_aio_read,
    .aio_write = emulate_aio_write
};

rbd_image_t write_nvme_pwrite(const char *dev, bool buffered)
{
    int buf = buffered ? 0 : O_DIRECT;
    int fd = open(dev, O_RDWR | buf);
    ssize_t sz = getsize64(fd);
    if (fd < 0 || sz < 0) {
	perror("nvme open");
	return NULL;
    }

    auto fri = new fake_rbd_image;
    fri->fd = fd;
    fri->vol_size = 10L * 1024 * 1024 * 1024;
    fri->dev_size = getsize64(fd);
    fri->ops = &ops_1;
    return (rbd_image_t) fri;
}

/* implementation 2
 */
static int aio_write_async1(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			    rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto p = (lsvd_completion *)c;

    int n = fri->dev_size / len;
    std::uniform_int_distribution<int> uni(0, n - 1);
    off_t nvme_offset = uni(rng) * len;

    auto closure = wrap([p,c]{
	    p->done = true;
	    p->cb(c, p->arg);
	    p->cv.notify_all();
	    return true;
	});
    auto aio = new aio_wrapper(fri->fd, (char*)buf, len, nvme_offset, call_wrapped, closure);
    return aio_write(&aio->aio);
}

static rbd_ops ops_2 = {
    .aio_readv = NULL,
    .aio_writev = NULL,
    .aio_read = emulate_aio_read,
    .aio_write = aio_write_async1
};

rbd_image_t write_nvme_async(const char *dev, bool buffered)
{
    fake_rbd_image *fri = (fake_rbd_image*)write_nvme_pwrite(dev, buffered);
    fri->ops = &ops_2;
    return (rbd_image_t) fri;
}

void aio_callback_thread(fake_rbd_image *fri)
{
    std::vector<aiocb*> my_aios;
    std::unique_lock lk(fri->m);

    lk.unlock();
    
    for (;;) {
	if (my_aios.size() > 0) {
	    aio_suspend(my_aios.data(), my_aios.size(), NULL);
	}
	else {
	    lk.lock();
	    fri->cv.wait(lk);
	    lk.unlock();
	}

	if (fri->closed)
	    return;
	lk.lock();
	if (fri->aios.size() > 0) {
	    my_aios.push_back(fri->aios.front());
	    fri->aios.pop();
	}
	lk.unlock();

	for (auto it = my_aios.begin(); it != my_aios.end(); it++) {
	    if (aio_error(*it) != EINPROGRESS) {
		aio_wrapper_done((*it)->aio_sigevent.sigev_value);
		it = my_aios.erase(it);
	    }
	}
    }
}

static int aio_write_async3(rbd_image_t image, uint64_t off, size_t len, const char *buf,
			    rbd_completion_t c)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    auto p = (lsvd_completion *)c;

    int n = fri->dev_size / len;
    std::uniform_int_distribution<int> uni(0, n - 1);
    off_t nvme_offset = uni(rng) * len;

    auto closure = wrap([p,c]{
	    p->done = true;
	    p->cb(c, p->arg);
	    p->cv.notify_all();
	    return true;
	});
    auto aio = new aio_wrapper(fri->fd, (char*)buf, len, nvme_offset, call_wrapped, closure);
    aio->aio.aio_sigevent.sigev_notify = SIGEV_NONE;
    aio_write(&aio->aio);

    std::unique_lock lk(fri->m);
    fri->aios.push(&aio->aio);
    fri->cv.notify_one();
    return 0;
}

static rbd_ops ops_3 = {
    .aio_readv = NULL,
    .aio_writev = NULL,
    .aio_read = emulate_aio_read,
    .aio_write = aio_write_async3
};

rbd_image_t write_nvme_async3(const char *dev, bool buffered)
{
    fake_rbd_image *fri = (fake_rbd_image*)write_nvme_pwrite(dev, buffered);
    fri->ops = &ops_3;
    fri->threads.push(std::thread(aio_callback_thread, fri));
    return (rbd_image_t) fri;
}

/* 11: write randomly to NVME w/ pwrite, buffered
 * 12: write randomly to NVME w/ pwrite, direct
 * 21: write randomly to NVME w/ aio+callback, buffered
 * 22: write randomly to NVME w/ aio+callback, direct
 */
extern "C" int rbd_open(rados_ioctx_t io, const char *name, rbd_image_t *image,
			const char *snap_name)
{
    int rv;
    auto [mode, nvme] = split_string(std::string(name), ":");
    bool flag;
    
    switch (mode[0]) {
    case '1':			// 11:<dev> : buffered, 12:<dev> : direct
	assert(mode[1] == '1' || mode[1] == '2');
	*image = write_nvme_pwrite(nvme.c_str(), mode[1] == '1');
	return (*image == NULL) ? -1 : 0;

    case '2':			// 21: buffered, 22: direct
	assert(mode[1] == '1' || mode[1] == '2');
	*image = write_nvme_async(nvme.c_str(), mode[1] == '1');
	return (*image == NULL) ? -1 : 0;

    case '3':
	assert(mode[1] == '1' || mode[1] == '2');
	*image = write_nvme_async3(nvme.c_str(), mode[1] == '1');
	return (*image == NULL) ? -1 : 0;
	
    }

    return -1;
}

extern "C" int rbd_close(rbd_image_t image)
{
    fake_rbd_image *fri = (fake_rbd_image*)image;
    fri->closed = true;
    close(fri->fd);
    fri->cv.notify_all();
    while (!fri->threads.empty()) {
	fri->threads.front().join();
	fri->threads.pop();
    }
#if 0
    for (auto t : fri->threads)
	t.join();
#endif
    return 0;
}

/* any following functions are stubs only
 */
extern "C" int rbd_invalidate_cache(rbd_image_t image)
{
    return 0;
}

/* These RBD functions are unimplemented and return errors
 */

extern "C" int rbd_create(rados_ioctx_t io, const char *name, uint64_t size,
                            int *order)
{
    return -1;
}
extern "C" int rbd_resize(rbd_image_t image, uint64_t size)
{
    return -1;
}

extern "C" int rbd_snap_create(rbd_image_t image, const char *snapname)
{
    return -1;
}
extern "C" int rbd_snap_list(rbd_image_t image, rbd_snap_info_t *snaps,
                               int *max_snaps)
{
    return -1;
}
extern "C" void rbd_snap_list_end(rbd_snap_info_t *snaps)
{
}
extern "C" int rbd_snap_remove(rbd_image_t image, const char *snapname)
{
    return -1;
}
extern "C" int rbd_snap_rollback(rbd_image_t image, const char *snapname)
{
    return -1;
}