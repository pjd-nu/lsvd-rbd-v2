#include "read_cache.h"

    void read_cache::evict(int n) {
        printf("\nEVICTING %d\n", n);
        // assert(!m.try_lock());       // m must be locked
        std::uniform_int_distribution<int> uni(0,super->units - 1);
        for (int i = 0; i < n; i++) {
            int j = uni(rng);
            while (flat_map[j].obj == 0 || in_use[j] > 0)
                j = uni(rng);
            auto oo = flat_map[j];
            flat_map[j] = (extmap::obj_offset){0, 0};
            map.erase(oo);
            free_blks.push_back(j);
        }
    }

    void read_cache::evict_thread(thread_pool<int> *p) {
        auto wait_time = std::chrono::milliseconds(500);
        auto t0 = std::chrono::system_clock::now();
        auto timeout = std::chrono::seconds(2);

        std::unique_lock<std::mutex> lk(m);

        while (p->running) {
            p->cv.wait_for(lk, wait_time);
            if (!p->running)
                return;

            int n = 0;
            if ((int)free_blks.size() < super->units / 16)
                n = super->units / 4 - free_blks.size();
            if (n)
                evict(n);

            if (!map_dirty)     // free list didn't change
                continue;

            /* write the map (a) immediately if we evict something, or 
             * (b) occasionally if the map is dirty
             */
            auto t = std::chrono::system_clock::now();
            if (n > 0 || (t - t0) > timeout) {
                lk.unlock();
                write_map();
                t0 = t;
                lk.lock();
                map_dirty = false;
            }
        }
    }

    char *read_cache::get_cacheline_buf(int n) {
        char *buf;
        int len = 65536;
        const int maxbufs = 48;
        if (buf_loc.size() < maxbufs) {
            buf = (char*)aligned_alloc(512, len);
            memset(buf, 0, len);
        }
        else {
            int j = buf_loc.front();
            buf_loc.pop();
            //printf("stealing %d\n", j);
            assert(buffer[j] != NULL);
            buf = buffer[j];
            buffer[j] = NULL;
            in_use[j]--;
        }
        buf_loc.push(n);
        assert(buf != NULL);
        return buf;
    }

    std::pair<size_t,size_t> read_cache::async_read(size_t offset, char *buf, size_t len,
                                        void (*cb)(void*), void *ptr) {
        lba_t base = offset/512, sectors = len/512, limit = base+sectors;
        size_t skip_len = 0, read_len = 0;
        extmap::obj_offset oo = {0, 0};

        std::shared_lock lk(omap->m);
        auto it = omap->map.lookup(base);
        if (it == omap->map.end() || it->base() >= limit)
            skip_len = len;
        else {
            auto [_base, _limit, _ptr] = it->vals(base, limit);
            if (_base > base) {
                skip_len = 512 * (_base - base);
                buf += skip_len;
            }
            read_len = 512 * (_limit - _base);
            oo = _ptr;
        }
        lk.unlock();

        if (read_len == 0)
            return std::make_pair(skip_len, read_len);

        extmap::obj_offset unit = {oo.obj, oo.offset / unit_sectors};
        sector_t blk_base = unit.offset * unit_sectors;
        sector_t blk_offset = oo.offset % unit_sectors;
        sector_t blk_top_offset = std::min({(int)(blk_offset+sectors),
                    round_up(blk_offset+1,unit_sectors),
                    (int)(blk_offset + (limit-base))});
        int n = -1;             // cache block number

        std::unique_lock lk2(m);
        bool in_cache = false;
        auto it2 = map.find(unit);
        if (it2 != map.end()) {
            n = it2->second;
            in_cache = true;
        }

        /* protection against random reads - read-around when hit rate is too low
         */
        bool use_cache = free_blks.size() > 0 && hit_stats.user * 3 > hit_stats.backend * 2;

        if (in_cache) {
            sector_t blk_in_ssd = super->base*8 + n*unit_sectors,
                start = blk_in_ssd + blk_offset,
                finish = start + (blk_top_offset - blk_offset);
            size_t bytes = 512 * (finish - start);

            a_bit[n] = true;
            hit_stats.user += bytes/512;

            if (buffer[n] != NULL) {
                lk2.unlock();
                memcpy(buf, buffer[n] + blk_offset*512, bytes);
                cb(ptr);
            }
            else if (written[n]) {
                auto closure = wrap([this, n, cb, ptr]{
                        cb(ptr);
                        in_use[n]--;
                        return true;
                    });
                in_use[n]++;
                auto eio = new e_iocb;
                e_io_prep_pread(eio, fd, buf, bytes, 512L*start, call_wrapped, closure);
                e_io_submit(ioctx, eio);
            }
            else {              // prior read is pending
                auto closure = wrap([this, n, buf, blk_offset, bytes, cb, ptr]{
                        memcpy(buf, buffer[n] + blk_offset*512, bytes);
                        cb(ptr);
                        return true;
                    });
                pending[n].push_back(closure);
            }
            read_len = bytes;
        }
        else if (use_cache) {
            u1++;
            /* assign a location in cache before we start reading (and while we're
             * still holding the lock)
             */
            map_dirty = true;
            n = free_blks.back();
            free_blks.pop_back();
            written[n] = false;
            in_use[n]++;
            map[unit] = n;
            flat_map[n] = unit;
            auto _buf = get_cacheline_buf(n);
            //printf("reading [%d] %p\n", n, _buf);

            hit_stats.backend += unit_sectors;
            sector_t sectors = blk_top_offset - blk_offset;
            hit_stats.user += sectors;
            lk2.unlock();

            off_t nvme_offset = (super->base*8 + n*unit_sectors) * 512L;
            off_t buf_offset = blk_offset * 512L;
            off_t bytes = 512L * sectors;

            auto write_done = wrap([this, n]{
                    written[n] = true;
                    return true;
                });

            auto read_done = wrap([this, n, write_done, nvme_offset, buf_offset, bytes,
                                   _buf, buf, cb, ptr]{
                                      std::unique_lock lk(m);
                                      memcpy(buf, _buf + buf_offset, bytes);
                                      buffer[n] = _buf;
                                      //printf("setting buffer[%d] = %p\n", n, _buf);

                                      std::vector<void*> v(std::make_move_iterator(pending[n].begin()),
                                                           std::make_move_iterator(pending[n].end()));
                                      pending[n].erase(pending[n].begin(), pending[n].end());
                                      lk.unlock();
                                      cb(ptr);
                                      for (auto p : v)
                                          call_wrapped(p);
                                      auto eio = new e_iocb;
                                      e_io_prep_pwrite(eio, fd, _buf, unit_sectors*512L,
                                                       nvme_offset, call_wrapped, write_done);
                                      e_io_submit(ioctx, eio);
                                      return true;
                                  });

            io->aio_read_num_object(unit.obj, _buf, 512L*unit_sectors,
                                    512L*blk_base, call_wrapped, read_done);
            read_len = bytes;
        }
        else {
            u0++;
            hit_stats.user += read_len / 512;
            hit_stats.backend += read_len / 512;
            lk2.unlock();
            io->aio_read_num_object(oo.obj, buf, read_len, 512L*oo.offset, cb, ptr);
            // read_len unchanged
        }
        return std::make_pair(skip_len, read_len);
    }

    void read_cache::write_map(void) {
        pwrite(fd, flat_map, 4096 * super->map_blocks, 4096L * super->map_start);
    }


    void read_cache::get_info(j_read_super **p_super, extmap::obj_offset **p_flat, 
                  std::vector<int> **p_free_blks, std::map<extmap::obj_offset,int> **p_map) {
        if (p_super != NULL)
            *p_super = super;
        if (p_flat != NULL)
            *p_flat = flat_map;
        if (p_free_blks != NULL)
            *p_free_blks = &free_blks;
        if (p_map != NULL)
            *p_map = &map;
    }

    void read_cache::do_add(extmap::obj_offset unit, char *buf) {
        std::unique_lock lk(m);
        char *_buf = (char*)aligned_alloc(512, 65536);
        memcpy(_buf, buf, 65536);
        int n = free_blks.back();
        free_blks.pop_back();
        written[n] = true;
        map[unit] = n;
        flat_map[n] = unit;
        off_t nvme_offset = (super->base*8 + n*unit_sectors)*512L;
        pwrite(fd, _buf, unit_sectors*512L, nvme_offset);
        write_map();
        free(_buf);
    }

    void read_cache::do_evict(int n) {
        std::unique_lock lk(m);
        evict(n);
    }
    void read_cache::reset(void) {
    }
