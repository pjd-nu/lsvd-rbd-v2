/*
misc_cache.h : include file which contains several of the important classes and structures
used directly by translate and cache classes. As the name suggests, contains mostly miscellaneous
functions and class/structure definitions:
	-thread_pool class for translate class (also utilized by caches)
	-cache_work structure for caches
	-sized_vector for caches
	-commonly used objmap structure modified with mutex from extent.h
See the documentation below for each specific class and structure
*/

#ifndef MISC_CACHE_H
#define MISC_CACHE_H

// thread_pool:	A helper class that is used by translate and cache layers to keep
//		track of which pool of threads is being used with its own mutex
//		Contains a template T queue, mutex, template std::thread queue, condition_variable,
//		and its own constructer and deconstructer.
template <class T>
class thread_pool {
public:
    std::queue<T> q;
    bool         running;
    std::mutex  *m;
    std::condition_variable cv;
    std::queue<std::thread> pool;
    
    thread_pool(std::mutex *_m) {
	running = true;
	m = _m;
    }
    ~thread_pool() {
	std::unique_lock lk(*m);
	running = false;
	cv.notify_all();
	lk.unlock();
	while (!pool.empty()) {
	    pool.front().join();
	    pool.pop();
	}
    }

// get_locked :	Returns the value for the front of the thread_pool queue
    bool get_locked(std::unique_lock<std::mutex> &lk, T &val);

// put_locked :	Appends work to the queue
    void put_locked(T work);

// put :	calls put_locked with an active mutex
    void put(T work);
};

// decode_offset_len : 	given template T *p which is the sum of buf + offset *p is pushed
//			back for vals until the defined end which is a sum of the offset, length, and buf
template<class T>
void decode_offset_len(char *buf, size_t offset, size_t len, std::vector<T> &vals);

// objmap:	a modified object map whose only difference is the containment of a mutex to
//		be used with the extmap::objmap used inside it. For documentation on extmap
//		see extent.h
class objmap {
public:
    std::shared_mutex m;
    extmap::objmap    map;
};

// throw_fs_error :	Throws a file_system error with the inputted message
void throw_fs_error(std::string msg);

// cache_work:	This is a structure for support in using the read_cache and write_cache objects
//		It contains callback to function, sectors for the caches, smartiov (see smartiov.h
//		for documentation), and a constructure for itself
struct cache_work {
public:
    uint64_t  lba;
    void    (*callback)(void*);
    void     *ptr;
    sector_t     sectors;
    smartiov  iovs;
    cache_work(sector_t _lba, const iovec *iov, int iovcnt,
	       void (*_callback)(void*), void *_ptr) : iovs(iov, iovcnt) {
	lba = _lba;
	sectors = iovs.bytes() / 512;
	callback = _callback;
	ptr = _ptr;
    }
};


/* convenience class, because we don't know cache size etc.
 * at cache object construction time.
 */
template <class T>
class sized_vector {
    std::vector<T> *elements;
public:
    ~sized_vector() {
        delete elements;
    }
    void init(int n) {
        elements = new std::vector<T>(n);
    }
    void init(int n, T val) {
        elements = new std::vector<T>(n, val);
    }
    T &operator[](int index) {
        return (*elements)[index];
    }
};

#endif