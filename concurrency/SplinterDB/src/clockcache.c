// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

#ifndef VERI_CLOCKCACHE

/*
 * clockcache.c --
 *
 *     This file contains the implementation for a concurrent clock cache.
 */
#include "platform.h"

#include "clockcache.h"
#include "allocator.h"
#include "io.h"
#include "task.h"

#include <stddef.h>
#include "util.h"

#include "poison.h"


/* invalid "pointers" used to indicate that the given page or lookup is
 * unmapped */
#define CC_UNMAPPED_ENTRY UINT32_MAX
#define CC_UNMAPPED_ADDR  UINT64_MAX

// Number of entries to clean/evict/get_free in a per-thread batch
#define CC_ENTRIES_PER_BATCH 64

/* number of events to poll for during clockcache_wait */
#define CC_DEFAULT_MAX_IO_EVENTS 32

/* ref count modulus */
#define CC_REF_COUNT_ROW PLATFORM_CACHELINE_SIZE
#define CC_REF_COUNT_MODULUS (CC_REF_COUNT_ROW * CC_REF_COUNT_ROW);

/*
 * clockcache_log, etc. are used to write an output of cache operations to a
 * log file for debugging purposes. If CC_LOG is set, then all output is
 * written, if ADDR_TRACING is set, then only operations which affect entries
 * with either entry_number TRACE_ENTRY or address TRACE_ADDR are written.
 *
 * clockcache_log_stream should be called between platform_open_log_stream
 * and platform_close_log_stream.
 *
 * Note: these are debug functions, so calling platform_get_tid() potentially
 * repeatedly is ok.
 */
#ifdef ADDR_TRACING
#define clockcache_log(addr, entry, message, ...)                       \
   do {                                                                 \
      if (addr == TRACE_ADDR || entry == TRACE_ENTRY) {                 \
         platform_handle_log(cc->logfile, "(%lu) "message,              \
               platform_get_tid(), ##__VA_ARGS__);                      \
      }                                                                 \
   } while(0)
#define clockcache_log_stream(addr, entry, message, ...)                \
   do {                                                                 \
      if (addr == TRACE_ADDR || entry == TRACE_ENTRY) {                 \
         platform_log_stream("(%lu) "message, platform_get_tid(),       \
               ##__VA_ARGS__);                                          \
      }                                                                 \
   } while(0)
#else
#ifdef CC_LOG
#define clockcache_log(addr, entry, message, ...)                       \
   do {                                                                 \
      (void)(addr);                                                     \
      platform_handle_log(cc->logfile, "(%lu) "message,                 \
               platform_get_tid(), ##__VA_ARGS__);                      \
   } while (0)

#define clockcache_log_stream(addr, entry, message, ...)                \
   platform_log_stream("(%lu) "message, platform_get_tid(),             \
               ##__VA_ARGS__);
#else
#define clockcache_log(addr, entry, message, ...) \
   do {                                           \
      (void)(addr);                               \
      (void)(entry);                              \
      (void)(message);                            \
   } while (0)
#define clockcache_log_stream(addr, entry, message, ...) \
   do {                                                  \
      (void)(addr);                                      \
      (void)(entry);                                     \
      (void)(message);                                   \
   } while (0)
#endif
#endif

#if defined CC_LOG || defined ADDR_TRACING
#define clockcache_open_log_stream() platform_open_log_stream()
#else
#define clockcache_open_log_stream()
#endif

#if defined CC_LOG || defined ADDR_TRACING
#define clockcache_close_log_stream() platform_close_log_stream(cc->logfile)
#else
#define clockcache_close_log_stream()
#endif


// clang-format off
page_handle *clockcache_alloc                (clockcache *cc, uint64 addr, page_type type);
bool         clockcache_dealloc              (clockcache *cc, uint64 addr, page_type type);
uint8        clockcache_get_allocator_ref    (clockcache *cc, uint64 addr);
page_handle *clockcache_get                  (clockcache *cc, uint64 addr, bool blocking, page_type type);
cache_async_result clockcache_get_async      (clockcache *cc, uint64 addr, page_type type, cache_async_ctxt *ctxt);
void         clockcache_async_done           (clockcache *cc, page_type type, cache_async_ctxt *ctxt);
void         clockcache_unget                (clockcache *cc, page_handle *page);
bool         clockcache_claim                (clockcache *cc, page_handle *page);
void         clockcache_unclaim              (clockcache *cc, page_handle *page);
void         clockcache_lock                 (clockcache *cc, page_handle *page);
void         clockcache_unlock               (clockcache *cc, page_handle *page);
void         clockcache_prefetch(clockcache *cc, uint64 addr, page_type type);
void         clockcache_mark_dirty           (clockcache *cc, page_handle *page);
void         clockcache_pin                  (clockcache *cc, page_handle *page);
void         clockcache_unpin                (clockcache *cc, page_handle *page);

void         clockcache_page_sync            (clockcache *cc, page_handle *page, bool is_blocking, page_type type);
void         clockcache_extent_sync          (clockcache *cc, uint64 addr, uint64 *pages_outstanding);

void         clockcache_flush                (clockcache *cc);
int          clockcache_evict_all            (clockcache *cc, bool ignore_pinned);
void         clockcache_wait                 (clockcache *cc);

uint64       clockcache_get_page_size        (const clockcache *cc);
uint64       clockcache_get_extent_size      (const clockcache *cc);

void         clockcache_assert_ungot         (clockcache *cc, uint64 addr);
void         clockcache_assert_noleaks       (clockcache *cc);
void         clockcache_assert_no_locks_held (clockcache *cc);
void         clockcache_print                (clockcache *cc);
bool         clockcache_page_valid           (clockcache *cc, uint64 addr);
void         clockcache_validate_page        (clockcache *cc, page_handle *page, uint64 addr);

void         clockcache_print_stats          (clockcache *cc);
void         clockcache_io_stats             (clockcache *cc, uint64 *read_bytes, uint64 *write_bytes);
void         clockcache_reset_stats          (clockcache *cc);

uint32       clockcache_count_dirty          (clockcache *cc);
uint16       clockcache_get_read_ref         (clockcache *cc, page_handle *page);

bool         clockcache_present              (clockcache *cc, page_handle *page);
static void  clockcache_enable_sync_get      (clockcache *cc, bool enabled);
allocator *  clockcache_allocator            (clockcache *cc);

static cache_ops clockcache_ops = {
   .page_alloc        = (page_alloc_fn)        clockcache_alloc,
   .page_dealloc      = (page_dealloc_fn)      clockcache_dealloc,
   .page_get_ref      = (page_get_ref_fn)      clockcache_get_allocator_ref,
   .page_get          = (page_get_fn)          clockcache_get,
   .page_get_async    = (page_get_async_fn)    clockcache_get_async,
   .page_async_done   = (page_async_done_fn)   clockcache_async_done,
   .page_unget        = (page_unget_fn)        clockcache_unget,
   .page_claim        = (page_claim_fn)        clockcache_claim,
   .page_unclaim      = (page_unclaim_fn)      clockcache_unclaim,
   .page_lock         = (page_lock_fn)         clockcache_lock,
   .page_unlock       = (page_unlock_fn)       clockcache_unlock,
   .page_prefetch     = (page_prefetch_fn)     clockcache_prefetch,
   .page_mark_dirty   = (page_mark_dirty_fn)   clockcache_mark_dirty,
   .page_pin          = (page_pin_fn)          clockcache_pin,
   .page_unpin        = (page_unpin_fn)        clockcache_unpin,
   .page_sync         = (page_sync_fn)         clockcache_page_sync,
   .extent_sync       = (extent_sync_fn)       clockcache_extent_sync,
   .flush             = (flush_fn)             clockcache_flush,
   .evict             = (evict_fn)             clockcache_evict_all,
   .cleanup           = (cleanup_fn)           clockcache_wait,
   .get_page_size     = (get_cache_size_fn)    clockcache_get_page_size,
   .get_extent_size   = (get_cache_size_fn)    clockcache_get_extent_size,
   .assert_ungot      = (assert_ungot_fn)      clockcache_assert_ungot,
   .assert_free       = (assert_free_fn)       clockcache_assert_no_locks_held,
   .assert_noleaks    = (assert_noleaks)       clockcache_assert_noleaks,
   .print             = (print_fn)             clockcache_print,
   .print_stats       = (print_fn)             clockcache_print_stats,
   .io_stats          = (io_stats_fn)          clockcache_io_stats,
   .reset_stats       = (reset_stats_fn)       clockcache_reset_stats,
   .page_valid        = (page_valid_fn)        clockcache_page_valid,
   .validate_page     = (validate_page_fn)     clockcache_validate_page,

   .count_dirty       = (count_dirty_fn)       clockcache_count_dirty,
   .page_get_read_ref = (page_get_read_ref_fn) clockcache_get_read_ref,

   .cache_present     = (cache_present_fn)     clockcache_present,
   .enable_sync_get   = (enable_sync_get_fn)   clockcache_enable_sync_get,
   .cache_allocator   = (cache_allocator_fn)   clockcache_allocator,
};
// clang-format on

/*
 *----------------------------------------------------------------------
 *
 * status and status constants
 *
 *----------------------------------------------------------------------
 */

#define CC_FREE        (1u<<0)
#define CC_ACCESSED    (1u<<1)
#define CC_CLEAN       (1u<<2)
#define CC_WRITEBACK   (1u<<3)
#define CC_LOADING     (1u<<4)
#define CC_WRITELOCKED (1u<<5)
#define CC_CLAIMED     (1u<<6)

/* Common status flag combinations */
#define CC_FREE_STATUS \
         (0 \
            | CC_FREE \
         )
#define CC_EVICTABLE_STATUS \
         (0 \
            | CC_CLEAN \
         )
#define CC_LOCKED_EVICTABLE_STATUS \
         (0 \
            | CC_CLEAN \
            | CC_CLAIMED \
            | CC_WRITELOCKED \
         )
#define CC_ACCESSED_STATUS \
         (0 \
            | CC_ACCESSED \
            | CC_CLEAN \
         )
#define CC_ALLOC_STATUS /* dirty */ \
         (0 \
            | CC_WRITELOCKED \
            | CC_CLAIMED \
         )
#define CC_CLEANABLE1_STATUS /* dirty */ \
         (0)
#define CC_CLEANABLE2_STATUS /* dirty */ \
         (0 \
            | CC_ACCESSED \
         )
#define CC_WRITEBACK1_STATUS \
         (0 \
            | CC_WRITEBACK \
         )
#define CC_WRITEBACK2_STATUS \
         (0 \
            | CC_ACCESSED \
            | CC_WRITEBACK \
         )
#define CC_READ_LOADING_STATUS \
         (0 \
            | CC_ACCESSED \
            | CC_CLEAN \
            | CC_LOADING \
         )
#define CC_WRITE_LOADING_STATUS \
         (0 \
            | CC_ACCESSED \
            | CC_CLEAN \
            | CC_LOADING \
            | CC_WRITELOCKED \
         )

/*----------------------------------------------------------------------
 *
 * clockcache_{set/clear/test}_flag --
 *
 *      Atomically sets, clears or tests the given flag
 *
 *----------------------------------------------------------------------
 */

static inline uint32
clockcache_set_flag(clockcache *cc, uint32 entry_number, uint32 flag)
{
   return flag & __sync_fetch_and_or(&cc->entry[entry_number].status, flag);
}

static inline uint32
clockcache_clear_flag(clockcache *cc, uint32 entry_number, uint32 flag)
{
   return flag & __sync_fetch_and_and(&cc->entry[entry_number].status, ~flag);
}

static inline uint32
clockcache_test_flag(clockcache *cc, uint32 entry_number, uint32 flag)
{
   return flag & cc->entry[entry_number].status;
}

#ifdef RECORD_ACQUISITION_STACKS
static void
clockcache_record_backtrace(clockcache *cc,
                            uint32 entry_number)
{
   int myhistindex
      = __sync_fetch_and_add(&cc->entry[entry_number].next_history_record, 1);
   myhistindex = myhistindex % 32;
   clockcache_entry *myEntry = &cc->entry[entry_number];

   myEntry->history[myhistindex].status = myEntry->status;
   myEntry->history[myhistindex].refcount = 0;
   for (threadid i = 0; i < next_i; i++)
      myEntry->history[myhistindex].refcount
         += cc->refcount[i * cc->cfg->page_capacity + entry_number];
   backtrace(myEntry->history[myhistindex].backtrace, 32);
}
#else
#define clockcache_record_backtrace(a,b)
#endif

/*
 *----------------------------------------------------------------------
 *
 * utility functions
 *
 *----------------------------------------------------------------------
 */

static inline uint64
clockcache_multiply_by_page_size(clockcache *cc,
                                 uint64      addr)
{
   return addr << cc->cfg->log_page_size;
}

static inline uint64
clockcache_divide_by_page_size(clockcache *cc,
                               uint64      addr)
{
   return addr >> cc->cfg->log_page_size;
}


static inline uint32
clockcache_lookup(clockcache *cc,
                  uint64      addr)
{
   uint64 lookup_no = clockcache_divide_by_page_size(cc, addr);
   return cc->lookup[lookup_no];
}

static inline clockcache_entry *
clockcache_lookup_entry(clockcache *cc,
                        uint64      addr)
{
   return &cc->entry[clockcache_lookup(cc, addr)];
}

static inline bool
clockcache_pages_share_extent(clockcache *cc,
                              uint64      left_addr,
                              uint64      right_addr)
{
   return left_addr / cc->cfg->extent_size == right_addr / cc->cfg->extent_size;
}

static inline clockcache_entry *
clockcache_page_to_entry(clockcache  *cc,
                         page_handle *page)
{
   return (clockcache_entry *)((char *)page - offsetof(clockcache_entry, page));
}

static inline uint32
clockcache_page_to_entry_number(clockcache  *cc,
                                page_handle *page)
{
   return clockcache_page_to_entry(cc, page) - cc->entry;
}

static inline uint32
clockcache_data_to_entry_number(clockcache *cc,
                                char       *data)
{
   return clockcache_divide_by_page_size(cc, data - cc->data);
}

__attribute__ ((unused)) static inline clockcache_entry *
clockcache_data_to_entry(clockcache *cc,
                         char       *data)
{
   return &cc->entry[clockcache_data_to_entry_number(cc, data)];
}

uint64
clockcache_get_page_size(const clockcache *cc)
{
   return cc->cfg->page_size;
}

uint64
clockcache_get_extent_size(const clockcache *cc)
{
   return cc->cfg->extent_size;
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_wait --
 *
 *      Does some work while waiting. Currently just polls for async IO
 *      completion.
 *
 *      This function needs to poll for async IO callback completion to avoid
 *      deadlock.
 *
 *-----------------------------------------------------------------------------
 */

void
clockcache_wait(clockcache *cc)
{
   io_cleanup(cc->io, CC_DEFAULT_MAX_IO_EVENTS);
}


/*
 *-----------------------------------------------------------------------------
 *
 * ref counts
 *
 *      Each entry has a distributed ref count. This ref count is striped
 *      across cache lines, so the ref count for entry 0 tid 0 is on a
 *      different cache line from both the ref count for entry 1 tid 0 and
 *      entry 0 tid 1. This reduces false sharing.
 *
 *      get_ref_internal converts an entry_number and tid to the index in
 *      cc->refcount where the ref count is stored.
 *
 *-----------------------------------------------------------------------------
 */

static inline uint32
clockcache_get_ref_internal(clockcache *cc,
                            uint32      entry_number)
{
   // 1D transpose to avoid false sharing
   uint32 block_modulus = entry_number % CC_REF_COUNT_MODULUS;
   uint32 column = block_modulus % CC_REF_COUNT_ROW;
   uint32 row    = block_modulus / CC_REF_COUNT_ROW;

   uint32 new_modulus = column * CC_REF_COUNT_ROW + row;

   return entry_number - block_modulus + new_modulus;
}

static inline uint16
clockcache_get_ref(clockcache *cc,
                   uint32      entry_number,
                   uint64      counter_no)
{
   counter_no %= CC_RC_WIDTH;
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   return cc->refcount[counter_no * cc->cfg->page_capacity + rc_number];
}

static inline void
clockcache_inc_ref(clockcache *cc,
                   uint32      entry_number,
                   threadid    counter_no)
{
   counter_no %= CC_RC_WIDTH;
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);

   __attribute__ ((unused))
   uint16 refcount = __sync_fetch_and_add(
         &cc->refcount[counter_no * cc->cfg->page_capacity + rc_number], 1);
   debug_assert(refcount != MAX_READ_REFCOUNT);
}

static inline void
clockcache_dec_ref(clockcache *cc,
                   uint32      entry_number,
                   threadid    counter_no)
{
   counter_no %= CC_RC_WIDTH;
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);

   __attribute__ ((unused))
   uint16 refcount = __sync_fetch_and_sub(
         &cc->refcount[counter_no * cc->cfg->page_capacity + rc_number], 1);
   debug_assert(refcount != 0);
}

static inline uint8
clockcache_get_pin(clockcache *cc,
                   uint32      entry_number)
{
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   return cc->pincount[rc_number];
}

static inline void
clockcache_inc_pin(clockcache *cc,
                   uint32      entry_number)
{
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   __attribute__ ((unused)) uint8 refcount
      = __sync_fetch_and_add(&cc->pincount[rc_number], 1);
   debug_assert(refcount != UINT8_MAX);
}

static inline void
clockcache_dec_pin(clockcache *cc,
                   uint32      entry_number)
{
   uint64 rc_number = clockcache_get_ref_internal(cc, entry_number);
   debug_assert(rc_number < cc->cfg->page_capacity);
   __attribute__ ((unused)) uint8 refcount
      = __sync_fetch_and_sub(&cc->pincount[rc_number], 1);
   debug_assert(refcount != 0);
}

void
clockcache_assert_no_refs(clockcache *cc)
{
  threadid i;
  volatile uint32 j;
  for (i = 0; i < (MAX_THREADS-1); i++) {
     for (j = 0; j < cc->cfg->page_capacity; j++) {
        if (clockcache_get_ref(cc, j, i) != 0) {
           clockcache_get_ref(cc, j, i);
        }
        platform_assert(clockcache_get_ref(cc, j, i) == 0);
     }
  }
}

void
clockcache_assert_no_refs_and_pins(clockcache *cc)
{
  threadid i;
  uint32 j;
  for (i = 0; i < MAX_THREADS; i++) {
     for (j = 0; j < cc->cfg->page_capacity; j++) {
       platform_assert(clockcache_get_ref(cc, j, i) == 0);
     }
  }
}

void
clockcache_assert_no_locks_held(clockcache *cc)
{
   uint64 i;
   clockcache_assert_no_refs_and_pins(cc);
   for (i = 0; i < cc->cfg->page_capacity; i++)
      debug_assert(!clockcache_test_flag(cc, i, CC_WRITELOCKED));
}

void
clockcache_assert_clean(clockcache *cc)
{
   uint64 i;

   for (i = 0; i < cc->cfg->page_capacity; i++)
      debug_assert(clockcache_test_flag(cc, i, CC_FREE)
            || clockcache_test_flag(cc, i, CC_CLEAN));
}

/*
 *----------------------------------------------------------------------
 *
 * page locking functions
 *
 *----------------------------------------------------------------------
 */

typedef enum {
   GET_RC_SUCCESS,
   GET_RC_CONFLICT,
   GET_RC_EVICTED,
   GET_RC_FLUSHING,
} get_rc;

/*
 *----------------------------------------------------------------------
 *
 *      clockcache_try_get_read
 *
 *      returns:
 *      - GET_RC_SUCCESS if a read lock was obtained
 *      - GET_RC_EVICTED if the entry was evicted
 *      - GET_RC_CONFLICT if another thread holds a write lock
 *
 *      does not block
 *
 *----------------------------------------------------------------------
 */

static get_rc
clockcache_try_get_read(clockcache *cc,
                        uint32      entry_number,
                        bool        set_access)
{
   const threadid tid = platform_get_tid();
   clockcache_record_backtrace(cc, entry_number);

   // first check if write lock is held
   uint32 cc_writing = clockcache_test_flag(cc, entry_number, CC_WRITELOCKED);
   if (UNLIKELY(cc_writing)) {
      return GET_RC_CONFLICT;
   }

   // then obtain the read lock
   clockcache_inc_ref(cc, entry_number, tid);

   // clockcache_test_flag returns 32 bits, not 1 (cannot use bool)
   uint32 cc_free = clockcache_test_flag(cc, entry_number, CC_FREE);
   cc_writing = clockcache_test_flag(cc, entry_number, CC_WRITELOCKED);
   if (LIKELY(!cc_free && !cc_writing)) {
      // test and test and set to reduce contention
      if (set_access && !clockcache_test_flag(cc, entry_number, CC_ACCESSED)) {
         clockcache_set_flag(cc, entry_number, CC_ACCESSED);
      }
      return GET_RC_SUCCESS;
   }

   // cannot hold the read lock (either write lock is held or entry has been
   // evicted), dec ref and return
   clockcache_dec_ref(cc, entry_number, tid);

   if (cc_free) {
      return GET_RC_EVICTED;
   }

   // must be cc_writing
   debug_assert(cc_writing);
   return GET_RC_CONFLICT;
}

/*
 *----------------------------------------------------------------------
 *
 *      clockcache_get_read
 *
 *      returns:
 *      - GET_RC_SUCCESS if a read lock was obtained
 *      - GET_RC_EVICTED if the entry was evicted
 *
 *      blocks if another thread holds a write lock
 *
 *----------------------------------------------------------------------
 */

static get_rc
clockcache_get_read(clockcache *cc,
                    uint32      entry_number)
{
   clockcache_record_backtrace(cc, entry_number);
   get_rc rc = clockcache_try_get_read(cc, entry_number, TRUE);

   uint64 wait = 1;
   while (rc == GET_RC_CONFLICT) {
      platform_sleep(wait);
      wait = wait > 1024 ? wait : 2 * wait;
      rc = clockcache_try_get_read(cc, entry_number, TRUE);
   }

   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 *      clockcache_try_get_claim
 *
 *      Attempts to upgrade a read lock to claim.
 *
 *      NOTE: A caller must release the read lock on GET_RC_CONFLICT before
 *      attempting try_get_claim again to avoid deadlock.
 *
 *      returns:
 *      - GET_RC_SUCCESS if a claim was obtained
 *      - GET_RC_CONFLICT if another thread holds a claim (or write lock)
 *
 *      does not block
 *
 *----------------------------------------------------------------------
 */

static get_rc
clockcache_try_get_claim(clockcache *cc,
                         uint32 entry_number)
{
   clockcache_record_backtrace(cc, entry_number);

   clockcache_log(0, entry_number,
         "try_get_claim: entry_number %u claimed: %u\n",
         entry_number,
         clockcache_test_flag(cc, entry_number, CC_CLAIMED));

   if (clockcache_set_flag(cc, entry_number, CC_CLAIMED)) {
      clockcache_log(0, entry_number, "return false\n", NULL);
      return GET_RC_CONFLICT;
   }

   return GET_RC_SUCCESS;
}

/*
 *----------------------------------------------------------------------
 *
 *      clockcache_get_write
 *
 *      Upgrades a claim to a write lock.
 *
 *      blocks:
 *      - while read locks are released
 *      - while write back completes
 *
 *      cannot fail
 *
 *      Note: does not wait on CC_LOADING. Caller must either ensure that
 *      CC_LOADING is not set prior to calling (e.g. via a prior call to
 *      clockcache_get).
 *
 *----------------------------------------------------------------------
 */

static void
clockcache_get_write(clockcache *cc,
                     uint32      entry_number)
{
   const threadid tid = platform_get_tid();

   debug_assert(clockcache_test_flag(cc, entry_number, CC_CLAIMED));
   __attribute__ ((unused)) uint32 was_writing = clockcache_set_flag(cc,
         entry_number, CC_WRITELOCKED);
   debug_assert(!was_writing);
   debug_assert(!clockcache_test_flag(cc, entry_number, CC_LOADING));

   /*
    * If the thread that wants a write lock holds > 1 refs, it means
    * it has some async lookups which have yielded after taking refs.
    * This is currently not allowed; because such a thread would
    * easily be able to upgrade to write lock and modify the page
    * under it's own yielded lookup.
    *
    * If threads do async lookups, they must leave the
    * compaction+incorporation (that needs write locking) to
    * background threads.
    */
   // FIXME: [aconway 2020-09-11] This assert doesn't work with less dist
   // locks, not sure if it's fixable
   //debug_assert(clockcache_get_ref(cc, entry_number, tid) == 1);
   // Wait for flushing to finish
   while (clockcache_test_flag(cc, entry_number, CC_WRITEBACK)) {
      clockcache_wait(cc);
   }

   // Wait for readers to finish
   for (threadid thr_i = 0; thr_i < CC_RC_WIDTH; thr_i++) {
      if (tid % CC_RC_WIDTH != thr_i) {
         while (clockcache_get_ref(cc, entry_number, thr_i)) {
            platform_sleep(1);
         }
      } else {
         // we have a single ref, so wait for others to drop
         while (clockcache_get_ref(cc, entry_number, thr_i) > 1) {
            platform_sleep(1);
         }
      }
   }

   clockcache_record_backtrace(cc, entry_number);
}

/*
 *----------------------------------------------------------------------
 *
 *      clockcache_try_get_write
 *
 *      Attempts to upgrade a claim to a write lock.
 *
 *      returns:
 *      - GET_RC_SUCCESS if the write lock was obtained
 *      - GET_RC_CONFLICT if another thread holds a read lock
 *
 *      blocks on write back
 *
 *      Note: does not wait on CC_LOADING. Caller must either ensure that
 *      CC_LOADING is not set prior to calling (e.g. via a prior call to
 *      clockcache_get).
 *
 *----------------------------------------------------------------------
 */

static get_rc
clockcache_try_get_write(clockcache *cc,
                         uint32      entry_number)
{
   threadid thr_i;
   threadid tid = platform_get_tid();
   get_rc rc;

   clockcache_record_backtrace(cc, entry_number);

   debug_assert(clockcache_test_flag(cc, entry_number, CC_CLAIMED));
   __attribute__ ((unused))
   uint32 was_writing = clockcache_set_flag(cc, entry_number, CC_WRITELOCKED);
   debug_assert(!was_writing);
   debug_assert(!clockcache_test_flag(cc, entry_number, CC_LOADING));

   // if flushing, then bail
   if (clockcache_test_flag(cc, entry_number, CC_WRITEBACK)) {
      rc = GET_RC_FLUSHING;
      goto failed;
   }

   // check for readers
   for (thr_i = 0; thr_i < CC_RC_WIDTH; thr_i++) {
      if (tid % CC_RC_WIDTH != thr_i) {
         if (clockcache_get_ref(cc, entry_number, thr_i)) {
            // there is a reader, so bail
            rc = GET_RC_CONFLICT;
            goto failed;
         }
      } else {
         // we have a single ref, so if > 1 bail
         if (clockcache_get_ref(cc, entry_number, thr_i) > 1) {
            // there is a reader, so bail
            rc = GET_RC_CONFLICT;
            goto failed;
         }
      }
   }

   return GET_RC_SUCCESS;

failed:
   was_writing = clockcache_clear_flag(cc, entry_number, CC_WRITELOCKED);
   debug_assert(was_writing);
   return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * writeback functions
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * clockcache_ok_to_writeback
 *
 *      Tests the entry to see if write back is possible. Used for test and
 *      test and set.
 *
 *----------------------------------------------------------------------
 */

static inline bool
clockcache_ok_to_writeback(clockcache *cc,
                           uint32      entry_number,
                           bool        with_access)
{
   uint32 status = cc->entry[entry_number].status;
   return status == CC_CLEANABLE1_STATUS
      || (with_access && status == CC_CLEANABLE2_STATUS);
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_try_set_writeback
 *
 *      Atomically sets the CC_WRITEBACK flag if the status permits; current
 *      status must be:
 *         -- CC_CLEANABLE1_STATUS (= 0)                  // dirty
 *         -- CC_CLEANABLE2_STATUS (= 0 | CC_ACCESSED)    // dirty
 *
 *----------------------------------------------------------------------
 */

static inline bool
clockcache_try_set_writeback(clockcache *cc,
                             uint32      entry_number,
                             bool        with_access)
{
   volatile uint32 *status = &cc->entry[entry_number].status;
   if (__sync_bool_compare_and_swap(status,
            CC_CLEANABLE1_STATUS, CC_WRITEBACK1_STATUS))
      return TRUE;

   if (with_access && __sync_bool_compare_and_swap(status,
            CC_CLEANABLE2_STATUS, CC_WRITEBACK2_STATUS))
      return TRUE;
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_write_callback --
 *
 *      Internal callback function to clean up after writing out a vector of
 *      blocks to disk.
 *
 *----------------------------------------------------------------------
 */

#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
__attribute__((no_sanitize("memory")))
#  endif
#endif
void
clockcache_write_callback(void            *metadata,
                          struct iovec    *iovec,
                          uint64           count,
                          platform_status  status)
{
   clockcache *cc = *(clockcache **)metadata;
   uint64 i;
   uint32 entry_number;
   clockcache_entry *entry;
   uint64 addr;
   __attribute__ ((unused)) uint32 debug_status;

   platform_assert_status_ok(status);
   platform_assert(count > 0);
   platform_assert(count <= cc->cfg->pages_per_extent);

   for (i = 0; i < count; i++) {
      entry_number
         = clockcache_data_to_entry_number(cc, (char *)iovec[i].iov_base);
      entry = &cc->entry[entry_number];
      addr = entry->page.disk_addr;

      clockcache_log(addr, entry_number,
            "write_callback i %lu entry %u addr %lu\n",
            i, entry_number, addr);

      debug_status = clockcache_set_flag(cc, entry_number, CC_CLEAN);
      debug_assert(!debug_status);
      debug_status = clockcache_clear_flag(cc, entry_number, CC_WRITEBACK);
      debug_assert(debug_status);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_batch_start_writeback --
 *
 *      Iterates through all pages in the batch and issues writeback for any
 *      which are cleanable.
 *
 *      Where possible, the write is extented to the extent, including pages
 *      outside the batch.
 *
 *      If is_urgent is set, pages with CC_ACCESSED are written back, otherwise
 *      they are not.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_batch_start_writeback(clockcache *cc,
                                 uint64      batch,
                                 bool        is_urgent)
{
   uint32 entry_no, next_entry_no;
   uint64 addr, first_addr, end_addr, i;
   const threadid tid = platform_get_tid();
   uint64 start_entry_no = batch * CC_ENTRIES_PER_BATCH;
   uint64 end_entry_no = start_entry_no + CC_ENTRIES_PER_BATCH;
   platform_status status;

   clockcache_entry *entry, *next_entry;

   debug_assert(tid < MAX_THREADS - 1);
   debug_assert(cc != NULL);
   debug_assert(batch < cc->cfg->page_capacity / CC_ENTRIES_PER_BATCH);

   clockcache_open_log_stream();
   clockcache_log_stream(0, 0, "batch_start_writeback: %lu, entries %lu-%lu\n",
         batch, start_entry_no, end_entry_no - 1);

   // Iterate through the entries in the batch and try to write out the extents.
   for (entry_no = start_entry_no; entry_no < end_entry_no; entry_no++) {
      entry = &cc->entry[entry_no];
      addr = entry->page.disk_addr;
      // test and test and set in the if condition
      if (clockcache_ok_to_writeback(cc, entry_no, is_urgent)
            && clockcache_try_set_writeback(cc, entry_no, is_urgent)) {
         debug_assert(clockcache_lookup(cc, addr) == entry_no);
         first_addr = entry->page.disk_addr;
         // walk backwards through extent to find first cleanable entry
         do {
            first_addr -= cc->cfg->page_size;
            if (clockcache_pages_share_extent(cc, first_addr, addr))
               next_entry_no = clockcache_lookup(cc, first_addr);
            else
               next_entry_no = CC_UNMAPPED_ENTRY;
         } while (next_entry_no != CC_UNMAPPED_ENTRY
               && clockcache_try_set_writeback(cc, next_entry_no, is_urgent));
         first_addr += cc->cfg->page_size;
         end_addr = entry->page.disk_addr;
         // walk forwards through extent to find last cleanable entry
         do {
            end_addr += cc->cfg->page_size;
            if (clockcache_pages_share_extent(cc, end_addr, addr))
               next_entry_no = clockcache_lookup(cc, end_addr);
            else
               next_entry_no = CC_UNMAPPED_ENTRY;
         } while (next_entry_no != CC_UNMAPPED_ENTRY
               && clockcache_try_set_writeback(cc, next_entry_no, is_urgent));

         io_async_req *req = io_get_async_req(cc->io, TRUE);
         void *req_metadata = io_get_metadata(cc->io, req);
         *(clockcache **)req_metadata = cc;
         struct iovec *iovec = io_get_iovec(cc->io, req);
         uint64 req_count =
            clockcache_divide_by_page_size(cc, end_addr - first_addr);
         req->bytes = clockcache_multiply_by_page_size(cc, req_count);

         if (cc->cfg->use_stats) {
            cc->stats[tid].page_writes[entry->type] += req_count;
            cc->stats[tid].writes_issued++;
         }

         for (i = 0; i < req_count; i++) {
            addr = first_addr + clockcache_multiply_by_page_size(cc, i);
            next_entry = clockcache_lookup_entry(cc, addr);
            next_entry_no = clockcache_lookup(cc, addr);

            clockcache_log_stream(addr, next_entry_no,
                  "flush: entry %u addr %lu\n",
                  next_entry_no, addr);
            iovec[i].iov_base = next_entry->page.data;
         }

         status = io_write_async(cc->io, req, clockcache_write_callback,
                                 req_count, first_addr);
         platform_assert_status_ok(status);
      }
   }
   clockcache_close_log_stream();
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_flush --
 *
 *      Issues writeback for all page in the cache.
 *
 *      Asserts that there are no pins, read locks, claims or write locks.
 *
 *-----------------------------------------------------------------------------
 */

void
clockcache_flush(clockcache *cc)
{
   // make sure all aio is complete first
   io_cleanup_all(cc->io);

   // there can be no references or pins or things won't flush
   //clockcache_assert_no_locks_held(cc); // take out for performance

   // clean all the pages
   for (uint32 flush_hand = 0;
         flush_hand < cc->cfg->page_capacity / CC_ENTRIES_PER_BATCH;
         flush_hand++)
      clockcache_batch_start_writeback(cc, flush_hand, TRUE);

   // make sure all aio is complete again
   io_cleanup_all(cc->io);

   clockcache_assert_clean(cc);
}

/*
 *----------------------------------------------------------------------
 *
 * eviction functions
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 *
 * clockcache_try_evict
 *
 *      Attempts to evict the page if it is evictable
 *
 *----------------------------------------------------------------------
 */

static void
clockcache_try_evict(clockcache *cc,
                     uint32      entry_number)
{
   clockcache_entry *entry = &cc->entry[entry_number];
   const threadid tid = platform_get_tid();

   /* store status for testing, then clear CC_ACCESSED */
   uint32 status = entry->status;
   /* T&T&S */
   if (clockcache_test_flag(cc, entry_number, CC_ACCESSED)) {
      clockcache_clear_flag(cc, entry_number, CC_ACCESSED);
   }

   /*
    * perform fast tests and quit if they fail */
   /* Note: this implicitly tests for:
    * CC_ACCESSED, CC_CLAIMED, CC_WRITELOCK, CC_WRITEBACK
    * Note: here is where we check that the evicting thread doesn't hold a read
    * lock itself.
    */
   if (status != CC_EVICTABLE_STATUS
         || clockcache_get_ref(cc, entry_number, tid)
         || clockcache_get_pin(cc, entry_number)) {
      goto out;
   }

   /* try to evict:
    * 1. try to read lock
    * 2. try to claim
    * 3. try to write lock
    * 4. verify still evictable
    * 5. clear lookup, disk_addr
    * 6. set status to CC_FREE_STATUS (clears claim and write lock)
    * 7. release read lock */

   /* 1. try to read lock */
   if (clockcache_try_get_read(cc, entry_number, FALSE) != GET_RC_SUCCESS) {
      goto out;
   }

   /* 2. try to claim */
   if (clockcache_try_get_claim(cc, entry_number) != GET_RC_SUCCESS) {
      goto release_ref;
   }

   /*
    * 3. try to write lock
    *      -- first check if loading
    */
   if (clockcache_test_flag(cc, entry_number, CC_LOADING)
         || clockcache_try_get_write(cc, entry_number) != GET_RC_SUCCESS) {
      goto release_claim;
   }

   /* 4. verify still evictable
    * redo fast tests in case another thread has changed the status before we
    * obtained the lock
    * note: do not re-check the ref count for the active thread, because
    * it acquired a read lock in order to lock the entry.
    */
   status = entry->status;
   if (status != CC_LOCKED_EVICTABLE_STATUS
         || clockcache_get_pin(cc, entry_number)) {
      goto release_write;
   }

   /* 5. clear lookup, disk addr */
   uint64 addr = entry->page.disk_addr;
   if (addr != CC_UNMAPPED_ADDR) {
      uint64 lookup_no = clockcache_divide_by_page_size(cc, addr);
      cc->lookup[lookup_no] = CC_UNMAPPED_ENTRY;
      entry->page.disk_addr = CC_UNMAPPED_ADDR;
   }
   __attribute__ ((unused)) uint32 debug_status =
      clockcache_test_flag(cc, entry_number, CC_WRITELOCKED | CC_CLAIMED);
   debug_assert(debug_status);

   /* 6. set status to CC_FREE_STATUS (clears claim and write lock) */
   entry->status = CC_FREE_STATUS;
   clockcache_log(addr, entry_number, "evict: entry %u addr %lu\n",
         entry_number, addr);

   /* 7. release read lock */
   goto release_ref;

release_write:
   debug_status = clockcache_clear_flag(cc, entry_number, CC_WRITELOCKED);
   debug_assert(debug_status);
release_claim:
   debug_status = clockcache_clear_flag(cc, entry_number, CC_CLAIMED);
   debug_assert(debug_status);
release_ref:
   clockcache_dec_ref(cc, entry_number, tid);
out:
   return;
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_evict_batch --
 *
 *      Evicts all evictable pages in the batch.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_evict_batch(clockcache *cc,
                       uint32      batch)
{
   debug_assert(cc != NULL);
   debug_assert(batch < cc->cfg->page_capacity / CC_ENTRIES_PER_BATCH);

   uint32 start_entry_no = batch * CC_ENTRIES_PER_BATCH;
   uint32 end_entry_no = start_entry_no + CC_ENTRIES_PER_BATCH;

   clockcache_log(0, 0, "evict_batch: %u, entries %u-%u\n",
         batch, start_entry_no, end_entry_no - 1);

   for (uint32 entry_no = start_entry_no; entry_no < end_entry_no; entry_no++) {
      clockcache_try_evict(cc, entry_no);
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_evict_all --
 *
 *      evicts all the pages.
 *
 *-----------------------------------------------------------------------------
 */

int
clockcache_evict_all(clockcache *cc, bool ignore_pinned_pages)
{
   uint32 evict_hand;
   uint32 i;

   if (!ignore_pinned_pages) {
      // there can be no references or pins or locks or it will block eviction
      clockcache_assert_no_locks_held(cc); // take out for performance
   }

   // evict all the pages
   for (evict_hand = 0; evict_hand < cc->cfg->batch_capacity; evict_hand++) {
      clockcache_evict_batch(cc, evict_hand);
      // Do it again for access bits
      clockcache_evict_batch(cc, evict_hand);
   }

   for (i = 0; i < cc->cfg->page_capacity; i++) {
      __attribute__ ((unused)) uint32 entry_no =
         clockcache_page_to_entry_number(cc, &cc->entry->page);
      // Every page should either be evicted or pinned.
      debug_assert(cc->entry[i].status == CC_FREE_STATUS ||
                  (ignore_pinned_pages &&
                   clockcache_get_pin(cc, entry_no)));
   }

   return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_move_hand --
 *
 *      Moves the clock hand forward cleaning and evicting a batch. Cleans
 *      "accessed" pages if is_urgent is set, for example when get_free_page
 *      has cycled through the cache already.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_move_hand(clockcache *cc,
                     bool        is_urgent)
{
   const threadid tid = platform_get_tid();
   volatile bool *evict_batch_busy;
   volatile bool *clean_batch_busy;
   uint64 cleaner_hand;

   /* move the hand a batch forward */
   uint64 evict_hand = cc->per_thread[tid].free_hand;
   __attribute__ ((unused)) bool was_busy = TRUE;
   if (evict_hand != CC_UNMAPPED_ENTRY) {
      evict_batch_busy = &cc->batch_busy[evict_hand];
      was_busy = __sync_bool_compare_and_swap(evict_batch_busy, TRUE, FALSE);
      debug_assert(was_busy);
   }
   do {
      evict_hand
         = __sync_add_and_fetch(&cc->evict_hand, 1) % cc->cfg->batch_capacity;
      evict_batch_busy = &cc->batch_busy[evict_hand];
      // clean the batch ahead
      cleaner_hand = (evict_hand + cc->cleaner_gap) % cc->cfg->batch_capacity;
      clean_batch_busy = &cc->batch_busy[cleaner_hand];
      if (__sync_bool_compare_and_swap(clean_batch_busy, FALSE, TRUE)) {
         clockcache_batch_start_writeback(cc, cleaner_hand, is_urgent);
         was_busy = __sync_bool_compare_and_swap(clean_batch_busy, TRUE, FALSE);
         debug_assert(was_busy);
      }
   } while (!__sync_bool_compare_and_swap(evict_batch_busy, FALSE, TRUE));

   clockcache_evict_batch(cc, evict_hand % cc->cfg->batch_capacity);
   cc->per_thread[tid].free_hand = evict_hand % cc->cfg->batch_capacity;
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_get_free_page --
 *
 *      returns a free page with given status and ref count.
 *
 *----------------------------------------------------------------------
 */

uint32
clockcache_get_free_page(clockcache *cc,
                         uint32      status,
                         bool        refcount,
                         bool        blocking)
{
   uint32 entry_no;
   uint64 num_passes = 0;
   const threadid tid = platform_get_tid();
   uint64 max_hand = cc->per_thread[tid].free_hand;
   clockcache_entry *entry;
   timestamp wait_start;

   debug_assert(tid < MAX_THREADS - 1);
   if (cc->per_thread[tid].free_hand == CC_UNMAPPED_ENTRY) {
      clockcache_move_hand(cc, FALSE);
   }

   /*
    * Debug builds can run on very high latency storage eg. Nimbus. Do
    * not give up after 3 passes on the cache. At least wait for the
    * max latency of an IO and keep making passes.
    */
   while (num_passes < 3 ||
          (blocking && !io_max_latency_elapsed(cc->io, wait_start))) {
      uint64 start_entry = cc->per_thread[tid].free_hand * CC_ENTRIES_PER_BATCH;
      uint64 end_entry = start_entry + CC_ENTRIES_PER_BATCH;
      for (entry_no = start_entry; entry_no < end_entry; entry_no++) {
         entry = &cc->entry[entry_no];
         if (entry->status == CC_FREE_STATUS &&
               __sync_bool_compare_and_swap(&entry->status, CC_FREE_STATUS,
                                                             CC_ALLOC_STATUS)) {
            if (refcount) {
               clockcache_inc_ref(cc, entry_no, tid);
            }
            entry->status = status;
            debug_assert(entry->page.disk_addr == CC_UNMAPPED_ADDR);
            return entry_no;
         }
      }

      clockcache_move_hand(cc, num_passes != 0);
      if (cc->per_thread[tid].free_hand < max_hand) {
         num_passes++;
         /*
          * The first pass doesn't really have a fair chance at having
          * looked at the entire cache, still it's ok to start
          * reckoning start time for max latency. Since it runs into
          * seconds, we'll make another complete pass in a tiny
          * fraction of the max latency.
          */
         if (num_passes == 1) {
            wait_start = platform_get_timestamp();
         } else {
            platform_yield();
         }
         clockcache_wait(cc);
      }
      max_hand = cc->per_thread[tid].free_hand;
   }
   if (blocking) {
      platform_log("cache locked (num_passes=%lu time=%lu nsecs)\n", num_passes,
                   platform_timestamp_elapsed(wait_start));
      clockcache_print(cc);
      platform_assert(0);
   }

   return CC_UNMAPPED_ENTRY;
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_config_init --
 *
 *      Initialize clockcache config values
 *
 *-----------------------------------------------------------------------------
 */

void clockcache_config_init(clockcache_config *cache_cfg,
                            uint64             page_size,
                            uint64             extent_size,
                            uint64             capacity,
                            char              *cache_logfile,
                            uint64             use_stats)
{
   int rc;
   ZERO_CONTENTS(cache_cfg);

   cache_cfg->page_size     = page_size;
   cache_cfg->extent_size   = extent_size;
   cache_cfg->capacity      = capacity;
   cache_cfg->log_page_size = 63 - __builtin_clzll(page_size);
   cache_cfg->page_capacity = capacity / page_size;
   cache_cfg->page_capacity /= CC_REF_COUNT_MODULUS;
   cache_cfg->page_capacity *= CC_REF_COUNT_MODULUS;
   cache_cfg->use_stats     = use_stats;

   rc = snprintf(cache_cfg->logfile, MAX_STRING_LENGTH, "%s", cache_logfile);
   platform_assert(rc < MAX_STRING_LENGTH);
}

platform_status
clockcache_init(clockcache           *cc,     // OUT
                clockcache_config    *cfg,    // IN
                io_handle            *io,     // IN
                allocator            *al,     // IN
                char                 *name,   // IN
                task_system          *ts,  // IN
                platform_heap_handle  hh,     // IN
                platform_heap_id      hid,    // IN
                platform_module_id    mid)    // IN
{
   int i;
   threadid thr_i;

   platform_assert(cc != NULL);
   ZERO_CONTENTS(cc);

   cc->cfg = cfg;
   cc->super.ops = &clockcache_ops;

   uint64 allocator_page_capacity
      = clockcache_divide_by_page_size(cc, allocator_get_capacity(al));
   uint64 debug_capacity
      = clockcache_multiply_by_page_size(cc, cc->cfg->page_capacity);
   cc->cfg->batch_capacity = cc->cfg->page_capacity / CC_ENTRIES_PER_BATCH;
   cc->cfg->cacheline_capacity
      = cc->cfg->page_capacity / PLATFORM_CACHELINE_SIZE;
   cc->cfg->pages_per_extent
      = clockcache_divide_by_page_size(cc, cc->cfg->extent_size);

   platform_assert(cc->cfg->page_capacity % PLATFORM_CACHELINE_SIZE == 0);
   platform_assert(cc->cfg->capacity == debug_capacity);
   platform_assert(cc->cfg->page_capacity % CC_ENTRIES_PER_BATCH == 0);

   /* Set the cleaner gap to 1/8 of page_capacity */
   /* FIXME: [aconway 2020-03-19]
    * The cleaner gap should really be a fixed number of batches, rather than a
    * fraction of the total cache capacity */
   cc->cleaner_gap = 512;

#if defined(CC_LOG) || defined(ADDR_TRACING)
   cc->logfile = platform_open_log_file(cfg->logfile, "w");
#else
   cc->logfile = NULL;
#endif
   clockcache_log(0, 0, "init: capacity %lu name %s\n",
                  cc->cfg->capacity, name);

   cc->al = al;
   cc->io = io;
   cc->heap_handle = hh;
   cc->heap_id = hid;

   /* lookup maps addrs to entries, entry contains the entries themselves */
   cc->lookup = TYPED_ARRAY_MALLOC(cc->heap_id, cc->lookup,
                                   allocator_page_capacity);
   if (!cc->lookup) {
      goto alloc_error;
   }
   for (i = 0; i < allocator_page_capacity; i++) {
      cc->lookup[i] = CC_UNMAPPED_ENTRY;
   }

   cc->entry = TYPED_ARRAY_ZALLOC(cc->heap_id, cc->entry,
                                  cc->cfg->page_capacity);
   if (!cc->entry) {
      goto alloc_error;
   }

   /* data must be aligned because of O_DIRECT */
   cc->bh = platform_buffer_create(cc->cfg->capacity, cc->heap_handle, mid);
   if (!cc->bh) {
      goto alloc_error;
   }
   cc->data = platform_buffer_getaddr(cc->bh);

   /* Set up the entries */
   for (i = 0; i < cc->cfg->page_capacity; i++) {
      cc->entry[i].page.data
         = cc->data + clockcache_multiply_by_page_size(cc, i);
      cc->entry[i].page.disk_addr = CC_UNMAPPED_ADDR;
      cc->entry[i].status = CC_FREE_STATUS;
   }

   /* Entry per-thread ref counts */
   size_t refcount_size = cc->cfg->page_capacity * CC_RC_WIDTH * sizeof(uint8);
   cc->rc_bh = platform_buffer_create(refcount_size, cc->heap_handle, mid);
   if (!cc->rc_bh) {
      goto alloc_error;
   }
   cc->refcount = platform_buffer_getaddr(cc->rc_bh);
   /* Separate ref counts for pins */
   cc->pincount = TYPED_ARRAY_ZALLOC(cc->heap_id, cc->pincount,
                                     cc->cfg->page_capacity);

   /* The hands and associated page */
   cc->free_hand = 0;
   cc->evict_hand = 1;
   for (thr_i = 0; thr_i < MAX_THREADS; thr_i++) {
      cc->per_thread[thr_i].free_hand = CC_UNMAPPED_ENTRY;
      cc->per_thread[thr_i].enable_sync_get = TRUE;
   }
   // FIXME: [yfogel 2020-03-12] investigate performance implication of
   // increasing to 8(64?) byte booleans, aligning, or perhaps interleaving the
   // order of the hand.
   cc->batch_busy =
      TYPED_ARRAY_ZALLOC(cc->heap_id, cc->batch_busy,
                         cc->cfg->page_capacity / CC_ENTRIES_PER_BATCH);
   if (!cc->batch_busy) {
      goto alloc_error;
   }
   cc->ts = ts;

   return STATUS_OK;

alloc_error:
   clockcache_deinit(cc);
   return STATUS_NO_MEMORY;
}

void
clockcache_deinit(clockcache *cc) // IN/OUT
{
   platform_assert(cc != NULL);

   /*
    * Check for non-null cause this is also used to clean up a failed
    * clockcache_init
    */
   if (cc->logfile) {
      clockcache_log(0, 0, "deinit %s\n", "");
#if defined(CC_LOG) || defined(ADDR_TRACING)
      platform_close_log_file(cc->logfile);
#endif
   }

   if (cc->rc_bh) {
      platform_buffer_destroy(cc->rc_bh);
   }

   platform_free(cc->heap_id, cc->entry);
   platform_free(cc->heap_id, cc->lookup);
   if (cc->bh) {
      platform_buffer_destroy(cc->bh);
   }
   cc->data = NULL;
   //clockcache_assert_noleaks(cc);
   platform_free_volatile(cc->heap_id, cc->batch_busy);
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_alloc --
 *
 *      Given a disk_addr, allocate entry in the cache and return its page with
 *      a write lock.
 *
 *----------------------------------------------------------------------
 */

page_handle *
clockcache_alloc(clockcache *cc, uint64 addr, page_type type)
{
   uint32            entry_no = clockcache_get_free_page(cc,
                                              CC_ALLOC_STATUS,
                                              TRUE,  // refcount
                                              TRUE); // blocking
   clockcache_entry *entry    = &cc->entry[entry_no];
   entry->page.disk_addr      = addr;
   entry->type                = type;
   if (cc->cfg->use_stats) {
      const threadid tid = platform_get_tid();
      cc->stats[tid].page_allocs[type]++;
   }
   uint64 lookup_no = clockcache_divide_by_page_size(cc, entry->page.disk_addr);
   cc->lookup[lookup_no] = entry_no;

   clockcache_log(entry->page.disk_addr,
                  entry_no,
                  "alloc: entry %u addr %lu\n",
                  entry_no,
                  entry->page.disk_addr);
   return &entry->page;
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_try_dealloc_page --
 *
 *      Evicts the page with address addr if it is in cache.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_try_dealloc_page(clockcache *cc,
                            uint64      addr)
{
   const threadid tid = platform_get_tid();
   while (TRUE) {
      uint32 entry_number = clockcache_lookup(cc, addr);
      if (entry_number == CC_UNMAPPED_ENTRY) {
         clockcache_log(addr, entry_number,
               "dealloc (uncached): entry %u addr %lu\n", entry_number, addr);
         return;
      }
      /*
       * in cache, so evict:
       * 1. read lock
       * 2. wait for loading
       * 3. claim
       * 4. write lock
       * 5. clear lookup, disk_addr
       * 6. set status to CC_FREE_STATUS (clears claim and write lock)
       * 7. release read lock
       */

      //platform_assert(clockcache_get_ref(cc, entry_number, tid) == 0);

      /* 1. read lock */
      if (clockcache_get_read(cc, entry_number) == GET_RC_EVICTED) {
         // raced with eviction, try again
         continue;
      }

      /* 2. wait for loading */
      while (clockcache_test_flag(cc, entry_number, CC_LOADING)) {
         clockcache_wait(cc);
      }

      clockcache_entry *entry = &cc->entry[entry_number];

      if (entry->page.disk_addr != addr) {
         // raced with eviction, try again
         clockcache_dec_ref(cc, entry_number, tid);
         continue;
      }

      /* 3. claim */
      if (clockcache_try_get_claim(cc, entry_number) != GET_RC_SUCCESS) {
         // failed to get claim, try again
         clockcache_dec_ref(cc, entry_number, tid);
         continue;
      }

      /* log only after steps that can fail */
      clockcache_log(addr, entry_number,
            "dealloc (cached): entry %u addr %lu\n", entry_number, addr);

      /* 4. write lock */
      clockcache_get_write(cc, entry_number);

      /* 5. clear lookup and disk addr; set status to CC_FREE_STATUS */
      uint64 lookup_no = clockcache_divide_by_page_size(cc, addr);
      cc->lookup[lookup_no] = CC_UNMAPPED_ENTRY;
      debug_assert(entry->page.disk_addr == addr);
      entry->page.disk_addr = CC_UNMAPPED_ADDR;

      /* 6. set status to CC_FREE_STATUS (clears claim and write lock) */
      entry->status = CC_FREE_STATUS;

      /* 7. release read lock */
      clockcache_dec_ref(cc, entry_number, tid);
      return;
   }
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_dealloc --
 *
 *      Lowers the allocator ref count on the extent with the given base
 *      address. If the ref count logically drops to 0 (1 in the allocator),
 *      any of those pages which are in cache are also freed and then the
 *      allocation is release (the allocator ref count is lowered to 0).
 *      If this drops to 0, the block is freed.
 *
 *----------------------------------------------------------------------
 */

bool
clockcache_dealloc(clockcache *cc,
                   uint64      addr,
                   page_type   type)
{
   debug_assert(addr % cc->cfg->extent_size == 0);
   const threadid tid = platform_get_tid();

   clockcache_log(addr, 0, "dealloc extent: addr %lu\n", addr);
   uint8 allocator_rc = allocator_dec_refcount(cc->al, addr);
   if (allocator_rc == 2) {
      // this means it is now 1, meaning not free but unref'd
      for (uint64 i = 0; i < cc->cfg->pages_per_extent; i++) {
         uint64 page_addr = addr + clockcache_multiply_by_page_size(cc, i);
         clockcache_try_dealloc_page(cc, page_addr);
      }
      allocator_rc = allocator_dec_refcount(cc->al, addr);
      debug_assert(allocator_rc == 1);
      if (cc->cfg->use_stats) {
         cc->stats[tid].page_deallocs[type] += cc->cfg->pages_per_extent;
      }
      return TRUE;
   }
   return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_get_allocator_ref --
 *
 *      Returns the allocator ref count of the addr.
 *
 *----------------------------------------------------------------------
 */

uint8
clockcache_get_allocator_ref(clockcache *cc, uint64 addr)
{
   return allocator_get_refcount(cc->al, addr);
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_get_internal --
 *
 *      Attempts to get a pointer to the page_handle for the page with
 *      address addr. If successful returns FALSE indicating no retries
 *      are needed, else TRUE indicating the caller needs to retry.
 *      Updates the "page" argument to the page_handle on sucess.
 *
 *      Will ask the caller to retry if we race with the eviction or if
 *      we have to evict an entry and race with someone else loading the
 *      entry.
 *      Blocks while the page is loaded into cache if necessary.
 *
 *----------------------------------------------------------------------
 */

static bool
clockcache_get_internal(clockcache *cc,              // IN
                        uint64      addr,            // IN
                        bool        blocking,        // IN
                        page_type   type,            // IN
                        page_handle **page)          // OUT
{
   debug_assert(addr % cc->cfg->page_size == 0);
   uint32 entry_number = CC_UNMAPPED_ENTRY;
   uint64 lookup_no = clockcache_divide_by_page_size(cc, addr);
   clockcache_entry *entry;
   __attribute__ ((unused)) platform_status status;
   __attribute__ ((unused)) uint64 base_addr = addr
      - addr % cc->cfg->extent_size;
   uint64 start, elapsed;
   const threadid tid = platform_get_tid();

   debug_assert(allocator_get_refcount(cc->al, base_addr) > 1);

   entry_number = clockcache_lookup(cc, addr);
   if (entry_number != CC_UNMAPPED_ENTRY) {
      if (blocking) {
         if (clockcache_get_read(cc, entry_number) != GET_RC_SUCCESS) {
            // this means we raced with eviction, start over
            clockcache_log(addr, entry_number,
                  "get (eviction race): entry %u addr %lu\n",
                  entry_number, addr);
            return TRUE;
         }
         if (cc->entry[entry_number].page.disk_addr != addr) {
            // this also means we raced with eviction and really lost
            clockcache_dec_ref(cc, entry_number, tid);
            return TRUE;
         }
      } else {
         switch(clockcache_try_get_read(cc, entry_number, TRUE)) {
            case GET_RC_CONFLICT:
               clockcache_log(addr, entry_number,
                     "get (locked -- non-blocking): entry %u addr %lu\n",
                     entry_number, addr);
               *page = NULL;
               return FALSE;
            case GET_RC_EVICTED:
               clockcache_log(addr, entry_number,
                     "get (eviction race): entry %u addr %lu\n",
                     entry_number, addr);
               return TRUE;
            case GET_RC_SUCCESS:
               if (cc->entry[entry_number].page.disk_addr != addr) {
                  // this also means we raced with eviction and really lost
                  clockcache_dec_ref(cc, entry_number, tid);
                  return TRUE;
               }
               break;
            default:
               platform_assert(0);
         }
      }

      while (clockcache_test_flag(cc, entry_number, CC_LOADING)) {
         clockcache_wait(cc);
      }
      entry = &cc->entry[entry_number];

      if (cc->cfg->use_stats) {
         cc->stats[tid].cache_hits[type]++;
      }
      clockcache_log(addr, entry_number,
            "get (cached): entry %u addr %lu rc %u\n",
            entry_number, addr, clockcache_get_ref(cc, entry_number, tid));
      *page = &entry->page;
      return FALSE;
   }
   /*
    * If a matching entry was not found, evict a page and load the requested
    * page from disk.
    */
   entry_number = clockcache_get_free_page(cc, CC_READ_LOADING_STATUS,
                                           TRUE,  // refcount
                                           TRUE); // blocking
   entry = &cc->entry[entry_number];
   /*
    * If someone else is loading the page and has reserved the lookup, let them
    * do it.
    */
   if (!__sync_bool_compare_and_swap(&cc->lookup[lookup_no],
            CC_UNMAPPED_ENTRY, entry_number)) {
      clockcache_dec_ref(cc, entry_number, tid);
      entry->status = CC_FREE_STATUS;
      clockcache_log(addr, entry_number,
            "get abort: entry: %u addr: %lu\n",
            entry_number, addr);
      return TRUE;
   }

   /* Set up the page */
   entry->page.disk_addr = addr;
   if (cc->cfg->use_stats) {
      start = platform_get_timestamp();
   }

   status = io_read(cc->io, entry->page.data, cc->cfg->page_size, addr);
   platform_assert_status_ok(status);

   if (cc->cfg->use_stats) {
      elapsed = platform_timestamp_elapsed(start);
      cc->stats[tid].cache_misses[type]++;
      cc->stats[tid].page_reads[type]++;
      cc->stats[tid].cache_miss_time_ns[type] += elapsed;
   }

   clockcache_log(addr, entry_number,
         "get (load): entry %u addr %lu\n",
         entry_number, addr);

   /* Clear the loading flag */
   clockcache_clear_flag(cc, entry_number, CC_LOADING);
   *page = &entry->page;
   return FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_get --
 *
 *      Returns a pointer to the page_handle for the page with address addr.
 *      Calls clockcachge_get_int till a retry is needed.
 *
 *      If blocking is set, then it blocks until the page is unlocked as well.
 *
 *      Returns with a read lock held.
 *
 *----------------------------------------------------------------------
 */

page_handle *
clockcache_get(clockcache *cc,
               uint64     addr,
               bool       blocking,
               page_type  type)
{
   bool retry;
   page_handle *handle;

   debug_assert(cc->per_thread[platform_get_tid()].enable_sync_get ||
                type == PAGE_TYPE_MEMTABLE || type == PAGE_TYPE_LOCK_NO_DATA);
   while (1) {
      retry = clockcache_get_internal(cc, addr, blocking, type, &handle);
      if (!retry) {
         return handle;
      }
   }
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_read_async_callback --
 *
 *    Async callback called after async read IO completes.
 *
 *----------------------------------------------------------------------
 */
static void
clockcache_read_async_callback(void            *metadata,
                               struct iovec    *iovec,
                               uint64           count,
                               platform_status  status)
{
   cache_async_ctxt *ctxt = *(cache_async_ctxt **)metadata;
   clockcache *cc = (clockcache *)ctxt->cc;

   platform_assert_status_ok(status);
   debug_assert(count == 1);

   uint32 entry_number
      = clockcache_data_to_entry_number(cc, (char *)iovec[0].iov_base);
   clockcache_entry *entry = &cc->entry[entry_number];
   uint64 addr = entry->page.disk_addr;
   debug_assert(addr != CC_UNMAPPED_ADDR);

   if (cc->cfg->use_stats) {
      threadid tid = platform_get_tid();
      cc->stats[tid].page_reads[entry->type]++;
      ctxt->stats.compl_ts = platform_get_timestamp();
   }

   debug_only uint32 lookup_entry_number;
   debug_code(lookup_entry_number = clockcache_lookup(cc, addr));
   debug_assert(lookup_entry_number == entry_number);
   debug_only uint32 was_loading
     = clockcache_clear_flag(cc, entry_number, CC_LOADING);
   debug_assert(was_loading);
   clockcache_log(addr, entry_number,
                  "async_get (load): entry %u addr %lu\n",
                  entry_number, addr);
   ctxt->status = status;
   ctxt->page = &entry->page;
   /* Call user callback function */
   ctxt->cb(ctxt);
   // can't deref ctxt anymore;
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_get_async --
 *
 *      Async version of clockcache_get(). This can return one of the
 *      following:
 *      - async_locked : page is write locked or being loaded
 *      - async_no_reqs : ran out of async requests (queue depth of device)
 *      - async_success : page hit in the cache. callback won't be called. Read
 *        lock is held on the page on return.
 *      - async_io_started : page miss in the cache. callback will be called
 *        when it's loaded. Page read lock is held after callback is called.
 *        The callback is not called on a thread context. It's the user's
 *        responsibility to call cache_async_done() on the thread context
 *        after the callback is done.
 *
 *----------------------------------------------------------------------
 */

cache_async_result
clockcache_get_async(clockcache        *cc,        // IN
                     uint64             addr,      // IN
                     page_type          type,      // IN
                     cache_async_ctxt  *ctxt)      // IN
{
#if SPLINTER_DEBUG
   static unsigned stress_retry;

   if (0 && ++stress_retry % 1000 == 0) {
      return async_locked;
   }
#endif

   debug_assert(addr % cc->cfg->page_size == 0);
   debug_assert((cache *)cc == ctxt->cc);
   uint32 entry_number = CC_UNMAPPED_ENTRY;
   uint64 lookup_no = clockcache_divide_by_page_size(cc, addr);
   clockcache_entry *entry;
   __attribute__ ((unused)) platform_status status;
   __attribute__ ((unused)) uint64 base_addr = addr
      - addr % cc->cfg->extent_size;
   const threadid tid = platform_get_tid();

   debug_assert(allocator_get_refcount(cc->al, base_addr) > 1);

   ctxt->page = NULL;
   entry_number = clockcache_lookup(cc, addr);
   if (entry_number != CC_UNMAPPED_ENTRY) {
      if (clockcache_try_get_read(cc, entry_number, TRUE) != GET_RC_SUCCESS) {
         /*
          * This means we raced with eviction, or there's another
          * thread that has the write lock. Either case, start over.
          */
         clockcache_log(addr, entry_number,
                        "get (eviction race): entry %u addr %lu\n",
                        entry_number, addr);
         return async_locked;
      }
      if (cc->entry[entry_number].page.disk_addr != addr) {
         // this also means we raced with eviction and really lost
         clockcache_dec_ref(cc, entry_number, tid);
         return async_locked;
      }
      if (clockcache_test_flag(cc, entry_number, CC_LOADING)) {
         /*
          * This is rare but when it happens, we could burn CPU retrying
          * the get operation until an IO is complete.
          */
         clockcache_dec_ref(cc, entry_number, tid);
         return async_locked;
      }
      entry = &cc->entry[entry_number];

      if (cc->cfg->use_stats) {
         cc->stats[tid].cache_hits[type]++;
      }
      clockcache_log(addr, entry_number,
            "get (cached): entry %u addr %lu rc %u\n",
            entry_number, addr, clockcache_get_ref(cc, entry_number, tid));
      ctxt->page = &entry->page;
      return async_success;
   }
   /*
    * If a matching entry was not found, evict a page and load the requested
    * page from disk.
    */
   entry_number = clockcache_get_free_page(cc, CC_READ_LOADING_STATUS,
                                           TRUE,   // refcount
                                           FALSE); // !blocking
   if (entry_number == CC_UNMAPPED_ENTRY) {
      return async_locked;
   }
   entry = &cc->entry[entry_number];
   /*
    * If someone else is loading the page and has reserved the lookup, let them
    * do it.
    */
   if (!__sync_bool_compare_and_swap(&cc->lookup[lookup_no],
            CC_UNMAPPED_ENTRY, entry_number)) {
      /*
       * This is rare but when it happens, we could burn CPU retrying
       * the get operation until an IO is complete.
       */
      entry->status = CC_FREE_STATUS;
      clockcache_dec_ref(cc, entry_number, tid);
      clockcache_log(addr, entry_number,
            "get retry: entry: %u addr: %lu\n",
            entry_number, addr);
      return async_locked;
   }

   /* Set up the page */
   entry->page.disk_addr = addr;
   entry->type = type;
   if (cc->cfg->use_stats) {
      ctxt->stats.issue_ts = platform_get_timestamp();
   }

   io_async_req *req = io_get_async_req(cc->io, FALSE);
   if (req == NULL) {
      cc->lookup[lookup_no] = CC_UNMAPPED_ENTRY;
      entry->page.disk_addr = CC_UNMAPPED_ADDR;
      entry->status = CC_FREE_STATUS;
      clockcache_dec_ref(cc, entry_number, tid);
      clockcache_log(addr, entry_number,
            "get retry(out of ioreq): entry: %u addr: %lu\n",
            entry_number, addr);
      return async_no_reqs;
   }
   req->bytes = clockcache_multiply_by_page_size(cc, 1);
   struct iovec *iovec = io_get_iovec(cc->io, req);
   iovec[0].iov_base = entry->page.data;
   void *req_metadata = io_get_metadata(cc->io, req);
   *(cache_async_ctxt **)req_metadata = ctxt;
   status = io_read_async(cc->io, req, clockcache_read_async_callback, 1, addr);
   platform_assert_status_ok(status);

   if (cc->cfg->use_stats) {
      cc->stats[tid].cache_misses[type]++;
   }

   return async_io_started;
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_async_done --
 *
 *    Called from thread context after the async callback has been invoked.
 *    Currently, it just updates cache miss stats.
 *
 *----------------------------------------------------------------------
 */
void
clockcache_async_done(clockcache       *cc,
                      page_type         type,
                      cache_async_ctxt *ctxt)
{
   if (cc->cfg->use_stats) {
      threadid tid = platform_get_tid();

      cc->stats[tid].cache_miss_time_ns[type] +=
         platform_timestamp_diff(ctxt->stats.issue_ts, ctxt->stats.compl_ts);
   }
}


void
clockcache_unget(clockcache *cc,
                 page_handle *page)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);
   const threadid tid = platform_get_tid();

   clockcache_record_backtrace(cc, entry_number);

   // T&T&S reduces contention
   if (!clockcache_test_flag(cc, entry_number, CC_ACCESSED)) {
      clockcache_set_flag(cc, entry_number, CC_ACCESSED);
   }

   clockcache_log(page->disk_addr, entry_number,
         "unget: entry %u addr %lu rc %u\n",
         entry_number, page->disk_addr,
         clockcache_get_ref(cc, entry_number, tid) - 1);
   clockcache_dec_ref(cc, entry_number, tid);
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_claim --
 *
 *      Upgrades a read lock to a claim. This function does not block and
 *      returns TRUE if the claim was successfully obtained.
 *
 *      A claimed node has the CC_CLAIMED bit set in its status vector.
 *
 *      NOTE: When a call to claim fails, the caller must drop and reobtain the
 *      readlock before trying to claim again to avoid deadlock.
 *
 *----------------------------------------------------------------------
 */

bool
clockcache_claim(clockcache *cc,
                 page_handle *page)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);

   clockcache_record_backtrace(cc, entry_number);
   clockcache_log(page->disk_addr, entry_number,
         "claim: entry %u addr %lu\n", entry_number, page->disk_addr);

   return clockcache_try_get_claim(cc, entry_number) == GET_RC_SUCCESS;
}

void
clockcache_unclaim(clockcache *cc,
                   page_handle *page)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);

   clockcache_record_backtrace(cc, entry_number);
   clockcache_log(page->disk_addr, entry_number,
         "unclaim: entry %u addr %lu\n",
         entry_number, page->disk_addr);

   __attribute__ ((unused)) uint32 status
      = clockcache_clear_flag(cc, entry_number, CC_CLAIMED);
   debug_assert(status);
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_lock --
 *
 *     Write locks a claimed page and blocks while any read locks are released.
 *
 *     The write lock is indicated by having the CC_WRITELOCKED flag set in
 *     addition to the CC_CLAIMED flag.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_lock(clockcache  *cc,
                page_handle *page)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);

   clockcache_record_backtrace(cc, entry_number);
   clockcache_log(page->disk_addr, entry_number,
         "lock: entry %u addr %lu\n",
         entry_number, page->disk_addr);
   clockcache_get_write(cc, entry_number);
}

void
clockcache_unlock(clockcache  *cc,
                  page_handle *page)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);

   clockcache_record_backtrace(cc, entry_number);
   clockcache_log(page->disk_addr, entry_number,
         "unlock: entry %u addr %lu\n",
         entry_number, page->disk_addr);
   __attribute__ ((unused)) uint32 was_writing
      = clockcache_clear_flag(cc, entry_number, CC_WRITELOCKED);
   debug_assert(was_writing);
}


/*----------------------------------------------------------------------
 *
 * clockcache_mark_dirty --
 *
 *      Marks the entry dirty.
 *
 *      FIXME: [aconway 2020-03-23]
 *      Maybe this should just get rolled into clockcache_lock?
 *
 *----------------------------------------------------------------------
 */

void
clockcache_mark_dirty(clockcache *cc,
                      page_handle *page)
{
   __attribute__ ((unused)) clockcache_entry *entry
      = clockcache_page_to_entry(cc, page);
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);

   clockcache_log(entry->page.disk_addr, entry_number,
         "mark_dirty: entry %u addr %lu\n",
         entry_number, entry->page.disk_addr);
   clockcache_clear_flag(cc, entry_number, CC_CLEAN);
   return;
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_pin --
 *
 *      Functionally equivalent to an anonymous read lock. Implemented using a
 *      special ref count.
 *
 *      A write lock must be held while pinning to avoid a race with eviction.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_pin(clockcache *cc,
               page_handle *page)
{
   __attribute__ ((unused)) clockcache_entry *entry
      = clockcache_page_to_entry(cc, page);
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);
   debug_assert(clockcache_test_flag(cc, entry_number, CC_WRITELOCKED));
   clockcache_inc_pin(cc, entry_number);

   clockcache_log(entry->page.disk_addr, entry_number,
         "pin: entry %u addr %lu\n",
         entry_number, entry->page.disk_addr);
}

void
clockcache_unpin(clockcache *cc,
                 page_handle *page)
{
   __attribute__ ((unused)) clockcache_entry *entry
      = clockcache_page_to_entry(cc, page);
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);
   clockcache_dec_pin(cc, entry_number);

   clockcache_log(entry->page.disk_addr, entry_number,
         "unpin: entry %u addr %lu\n",
         entry_number, entry->page.disk_addr);
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_page_sync --
 *
 *      Asynchronously syncs the page. Currently there is no way to check when
 *      the writeback has completed.
 *
 *-----------------------------------------------------------------------------
 */

void
clockcache_page_sync(clockcache  *cc,
                     page_handle *page,
                     bool         is_blocking,
                     page_type    type)
{
   uint32 entry_number = clockcache_page_to_entry_number(cc, page);
   io_async_req *req;
   struct iovec *iovec;
   uint64 addr = page->disk_addr;
   const threadid tid = platform_get_tid();
   platform_status status;

   if (!clockcache_try_set_writeback(cc, entry_number, TRUE)) {
      platform_assert(clockcache_test_flag(cc, entry_number, CC_CLEAN));
      return;
   }

   if (cc->cfg->use_stats) {
      cc->stats[tid].page_writes[type]++;
      cc->stats[tid].syncs_issued++;
   }

   if (!is_blocking) {
      req = io_get_async_req(cc->io, TRUE);
      void *req_metadata = io_get_metadata(cc->io, req);
      *(clockcache **)req_metadata = cc;
      uint64 req_count = 1;
      req->bytes = clockcache_multiply_by_page_size(cc, req_count);
      iovec = io_get_iovec(cc->io, req);
      iovec[0].iov_base = page->data;
      status = io_write_async(cc->io, req, clockcache_write_callback,
                              req_count, addr);
      platform_assert_status_ok(status);
   } else {
      status = io_write(cc->io, page->data, cc->cfg->page_size, addr);
      platform_assert_status_ok(status);
      clockcache_log(addr, entry_number,
            "page_sync write entry %u addr %lu\n", entry_number, addr);
      __attribute__ ((unused)) uint8 rc;
      rc = clockcache_set_flag(cc, entry_number, CC_CLEAN);
      debug_assert(!rc);
      rc = clockcache_clear_flag(cc, entry_number, CC_WRITEBACK);
      debug_assert(rc);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * clockcache_sync_callback --
 *
 *      internal callback for clockcache_extent_sync which decrements the pages
 *      outstanding counter
 *
 *----------------------------------------------------------------------
 */

typedef struct clockcache_sync_callback_req {
   clockcache *cc;
   uint64     *pages_outstanding;
} clockcache_sync_callback_req;

#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
__attribute__((no_sanitize("memory")))
#  endif
#endif
void
clockcache_sync_callback(void            *arg,
                         struct iovec    *iovec,
                         uint64           count,
                         platform_status  status)
{
   clockcache_sync_callback_req *req
      = (clockcache_sync_callback_req *)arg;
   uint64 pages_written = clockcache_divide_by_page_size(req->cc, count);
   clockcache_write_callback(req->cc, iovec, count, status);
   __sync_fetch_and_sub(req->pages_outstanding, pages_written);
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_extent_sync --
 *
 *      Asynchronously syncs the extent.
 *
 *      Adds the number of pages issued writeback to the coutner pointered to
 *      by pages_outstanding. When the writes complete, a callback subtracts
 *      them off, so that the caller may track how many pages are in writeback.
 *
 *      Assumes all pages in the extent are clean or cleanable
 *
 *-----------------------------------------------------------------------------
 */

void
clockcache_extent_sync(clockcache *cc,
                       uint64      addr,
                       uint64     *pages_outstanding)
{
   uint64 i;
   uint32 entry_number;
   uint64 req_count = 0;
   uint64 req_addr;
   uint64 page_addr;
   io_async_req *io_req;
   struct iovec *iovec;
   platform_status status;

   for (i = 0; i < cc->cfg->pages_per_extent; i++) {
      page_addr = addr + clockcache_multiply_by_page_size(cc, i);
      entry_number = clockcache_lookup(cc, page_addr);
      if (entry_number != CC_UNMAPPED_ENTRY
            && clockcache_try_set_writeback(cc, entry_number, TRUE)) {
         if (req_count == 0) {
            req_addr = page_addr;
            io_req = io_get_async_req(cc->io, TRUE);
            clockcache_sync_callback_req *cc_req
               = (clockcache_sync_callback_req *)io_get_metadata(cc->io, io_req);
            cc_req->cc = cc;
            cc_req->pages_outstanding = pages_outstanding;
            iovec = io_get_iovec(cc->io, io_req);
         }
         iovec[req_count++].iov_base = cc->entry[entry_number].page.data;
      } else {
         // ALEX: There is maybe a race with eviction with this assertion
         debug_assert(entry_number == CC_UNMAPPED_ENTRY
               || clockcache_test_flag(cc, entry_number, CC_CLEAN));
         if (req_count != 0) {
            __sync_fetch_and_add(pages_outstanding, req_count);
            io_req->bytes = clockcache_multiply_by_page_size(cc, req_count);
            status = io_write_async(cc->io, io_req, clockcache_sync_callback,
                                    req_count, req_addr);
            platform_assert_status_ok(status);
            req_count = 0;
         }
      }
   }
   if (req_count != 0) {
      __sync_fetch_and_add(pages_outstanding, req_count);
      status = io_write_async(cc->io, io_req, clockcache_sync_callback,
                              req_count, req_addr);
      platform_assert_status_ok(status);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_prefetch_callback --
 *
 *      Internal callback function to clean up after prefetching a collection
 *      of pages from the device.
 *
 *----------------------------------------------------------------------
 */

#if defined(__has_feature)
#   if __has_feature(memory_sanitizer)
__attribute__((no_sanitize("memory")))
#   endif
#endif
void
clockcache_prefetch_callback(void *          metadata,
                             struct iovec *  iovec,
                             uint64          count,
                             platform_status status)
{
   clockcache *      cc        = *(clockcache **)metadata;
   page_type         type      = PAGE_TYPE_INVALID;
   debug_only uint64 last_addr = CC_UNMAPPED_ADDR;

   platform_assert_status_ok(status);
   platform_assert(count > 0);
   platform_assert(count <= cc->cfg->pages_per_extent);

   for (uint64 page_off = 0; page_off < count; page_off++) {
      uint32 entry_no =
         clockcache_data_to_entry_number(cc, (char *)iovec[page_off].iov_base);
      clockcache_entry *entry = &cc->entry[entry_no];
      if (page_off != 0) {
         debug_assert(type == entry->type);
      } else {
         type = entry->type;
      }
      debug_only uint32 was_loading =
         clockcache_clear_flag(cc, entry_no, CC_LOADING);
      debug_assert(was_loading);

      debug_code(int64 addr = entry->page.disk_addr);
      debug_assert(addr != CC_UNMAPPED_ADDR);
      debug_assert(addr == last_addr + cc->cfg->page_size ||
                   last_addr == CC_UNMAPPED_ADDR);
      debug_code(last_addr = addr);
      debug_assert(entry_no == clockcache_lookup(cc, addr));
   }

   if (cc->cfg->use_stats) {
      threadid tid = platform_get_tid();
      cc->stats[tid].page_reads[type] += count;
      cc->stats[tid].prefetches_issued[type]++;
   }
}

/*
 *-----------------------------------------------------------------------------
 *
 * clockcache_prefetch --
 *
 *      prefetch asynchronously loads the extent with given base address
 *
 *-----------------------------------------------------------------------------
 */


void
clockcache_prefetch(clockcache *cc, uint64 base_addr, page_type type)
{
   io_async_req *req;
   struct iovec *iovec;
   uint64        pages_per_extent = cc->cfg->pages_per_extent;
   uint64        pages_in_req     = 0;
   uint64        req_start_addr   = CC_UNMAPPED_ADDR;
   threadid      tid              = platform_get_tid();

   debug_assert(base_addr % cc->cfg->extent_size == 0);

   for (uint64 page_off = 0; page_off < pages_per_extent; page_off++) {
      uint64 addr = base_addr + clockcache_multiply_by_page_size(cc, page_off);
      uint32 entry_no = clockcache_lookup(cc, addr);
      get_rc get_read_rc;
      if (entry_no != CC_UNMAPPED_ENTRY) {
         get_read_rc = clockcache_try_get_read(cc, entry_no, TRUE);
      } else {
         get_read_rc = GET_RC_EVICTED;
      }

      switch (get_read_rc) {
         case GET_RC_SUCCESS:
            clockcache_dec_ref(cc, entry_no, tid);
            // fallthrough
         case GET_RC_CONFLICT:
            // in cache, issue IO req if started
            if (pages_in_req != 0) {
               req->bytes = clockcache_multiply_by_page_size(cc, pages_in_req);
               platform_status rc = io_read_async(cc->io,
                                                  req,
                                                  clockcache_prefetch_callback,
                                                  pages_in_req,
                                                  req_start_addr);
               platform_assert_status_ok(rc);
               pages_in_req   = 0;
               req_start_addr = CC_UNMAPPED_ADDR;
            }
            clockcache_log(addr,
                           entry_no,
                           "prefetch (cached): entry %u addr %lu\n",
                           entry_no,
                           addr);
            break;
         case GET_RC_EVICTED: {
            // need to prefetch
            uint32 free_entry_no = clockcache_get_free_page(
               cc, CC_READ_LOADING_STATUS, FALSE, TRUE);
            clockcache_entry *entry = &cc->entry[free_entry_no];
            entry->page.disk_addr   = addr;
            entry->type             = type;
            uint64 lookup_no        = clockcache_divide_by_page_size(cc, addr);
            if (__sync_bool_compare_and_swap(
                   &cc->lookup[lookup_no], CC_UNMAPPED_ENTRY, free_entry_no)) {
               if (pages_in_req == 0) {
                  debug_assert(req_start_addr == CC_UNMAPPED_ADDR);
                  // start a new IO req
                  req                          = io_get_async_req(cc->io, TRUE);
                  void *req_metadata           = io_get_metadata(cc->io, req);
                  *(clockcache **)req_metadata = cc;
                  iovec                        = io_get_iovec(cc->io, req);
                  req_start_addr               = addr;
               }
               iovec[pages_in_req++].iov_base = entry->page.data;
               clockcache_log(addr,
                              entry_no,
                              "prefetch (load): entry %u addr %lu\n",
                              entry_no,
                              addr);
            } else {
               /*
                * someone else is already loading this page, release the free
                * entry and retry
                */
               entry->page.disk_addr = CC_UNMAPPED_ADDR;
               entry->status         = CC_FREE_STATUS;
               page_off--;
            }
            break;
         }
         default:
            platform_assert(0);
      }
   }
   // issue IO req if started
   if (pages_in_req != 0) {
      req->bytes         = clockcache_multiply_by_page_size(cc, pages_in_req);
      platform_status rc = io_read_async(cc->io,
                                         req,
                                         clockcache_prefetch_callback,
                                         pages_in_req,
                                         req_start_addr);
      pages_in_req       = 0;
      req_start_addr     = CC_UNMAPPED_ADDR;
      platform_assert_status_ok(rc);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * clockcache_print --
 *
 *      Prints a bitmap representation of the cache.
 *
 *----------------------------------------------------------------------
 */

void
clockcache_print(clockcache *cc)
{
   uint64 i;
   uint32 status;
   uint16 refcount;
   threadid thr_i;

   platform_open_log_stream();
   platform_log_stream("************************** CACHE CONTENTS "
                       "**************************\n");
   for (i = 0; i < cc->cfg->page_capacity; i++) {
      if (i != 0 && i % 16 == 0)
         platform_log_stream("\n");
      if (i % CC_ENTRIES_PER_BATCH == 0)
         platform_log_stream("Word %lu entries %lu-%lu\n", i / CC_ENTRIES_PER_BATCH, i, i + 63);
      status = cc->entry[i].status;
      refcount = 0;
      for (thr_i = 0; thr_i < CC_RC_WIDTH; thr_i++) {
         refcount += clockcache_get_ref(cc, i, thr_i);
      }
      platform_log_stream("0x%02x-%u ", status, refcount);
   }

   platform_log_stream("\n\n");
   platform_close_log_stream(stdout);
   return;
}

bool
clockcache_page_valid(clockcache *cc,
                      uint64      addr)
{
   if (addr % cc->cfg->page_size != 0)
      return FALSE;
   uint64 base_addr = addr - addr % cc->cfg->extent_size;
   if (addr < allocator_get_capacity(cc->al))
      return base_addr != 0 && allocator_get_refcount(cc->al, base_addr) != 0;
   else
      return FALSE;
}

void
clockcache_validate_page(clockcache  *cc,
                         page_handle *page,
                         uint64       addr)
{
   debug_assert(clockcache_page_valid(cc, addr));
   debug_assert(page->disk_addr == addr);
   debug_assert(!clockcache_test_flag(cc, clockcache_page_to_entry_number(cc, page), CC_FREE));
}

void
clockcache_assert_ungot(clockcache *cc,
                        uint64      addr)
{
   __attribute__ ((unused)) uint32 entry_number = clockcache_lookup(cc, addr);
   const threadid tid = platform_get_tid();
   if (entry_number != CC_UNMAPPED_ENTRY) {
      __attribute__ ((unused)) uint16 ref_count
                                    = clockcache_get_ref(cc, entry_number, tid);
      debug_assert(ref_count == 0);
   }
}

void
clockcache_assert_noleaks(clockcache *cc)
{
   if (!cc->cfg->use_stats) {
      return;
   }
   page_type type;
   uint64 i, allocs[NUM_PAGE_TYPES] = {0}, deallocs[NUM_PAGE_TYPES] = {0};

   const char *page_type_strings[NUM_PAGE_TYPES] = {
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_TRUNK),
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_BRANCH),
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_MEMTABLE),
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_FILTER),
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_LOG),
      SET_ARRAY_INDEX_TO_STRINGIFY(PAGE_TYPE_MISC),
   };

   for (i = 0; i < MAX_THREADS; i++) {
      for (type = 0; type < NUM_PAGE_TYPES; type++) {
         allocs[type] += cc->stats[i].page_allocs[type];
         deallocs[type] += cc->stats[i].page_deallocs[type];
      }
   }

   bool deallocs_match = TRUE;
   for (type = 0; type < NUM_PAGE_TYPES; type++) {
      if (type == PAGE_TYPE_LOG) {
         continue;
      }
      if (allocs[type] != deallocs[type]) {
         platform_log("%s: allocs %lu deallocs %lu\n",
                      page_type_strings[type],
                      allocs[PAGE_TYPE_TRUNK],
                      deallocs[PAGE_TYPE_TRUNK]);
         deallocs_match = FALSE;
      }
   }
   if (!deallocs_match) {
      //allocator_print_allocated(cc->al);
   }
   platform_assert(deallocs_match);
}

void
clockcache_io_stats(clockcache *cc,
                    uint64     *read_bytes,
                    uint64     *write_bytes)
{
  *read_bytes = 0;
  *write_bytes = 0;

   if (!cc->cfg->use_stats) {
      return;
   }

   uint64 read_pages = 0;
   uint64 write_pages = 0;
   for (uint64 i = 0; i < MAX_THREADS; i++) {
      for (page_type type = 0; type < NUM_PAGE_TYPES; type++) {
         write_pages += cc->stats[i].page_writes[type];
         read_pages += cc->stats[i].page_reads[type];
      }
   }

   *write_bytes = write_pages * 4 * KiB;
   *read_bytes = read_pages * 4 * KiB;
}

void
clockcache_print_stats(clockcache *cc)
{
   uint64 i;
   page_type type;
   cache_stats global_stats;

   if (!cc->cfg->use_stats) {
      return;
   }

   uint64 page_writes = 0;
   ZERO_CONTENTS(&global_stats);
   for (i = 0; i < MAX_THREADS; i++) {
      for (type = 0; type < NUM_PAGE_TYPES; type++) {
         global_stats.cache_hits[type] += cc->stats[i].cache_hits[type];
         global_stats.cache_misses[type] += cc->stats[i].cache_misses[type];
         global_stats.cache_miss_time_ns[type] +=
            cc->stats[i].cache_miss_time_ns[type];
         global_stats.page_allocs[type] += cc->stats[i].page_allocs[type];
         global_stats.page_deallocs[type] += cc->stats[i].page_deallocs[type];
         global_stats.page_writes[type] += cc->stats[i].page_writes[type];
         page_writes += cc->stats[i].page_writes[type];
         global_stats.page_reads[type] += cc->stats[i].page_reads[type];
         global_stats.prefetches_issued[type] +=
            cc->stats[i].prefetches_issued[type];
      }
      global_stats.writes_issued += cc->stats[i].writes_issued;
      global_stats.syncs_issued += cc->stats[i].syncs_issued;
   }

   fraction miss_time[NUM_PAGE_TYPES];
   fraction avg_prefetch_pages[NUM_PAGE_TYPES];
   fraction avg_write_pages;

   for (type = 0; type < NUM_PAGE_TYPES; type++) {
      miss_time[type] =
         init_fraction(global_stats.cache_miss_time_ns[type], SEC_TO_NSEC(1));
      avg_prefetch_pages[type] = init_fraction(
         global_stats.page_reads[type] - global_stats.cache_misses[type],
         global_stats.prefetches_issued[type]);
   }
   avg_write_pages = init_fraction(page_writes - global_stats.syncs_issued,
                                   global_stats.writes_issued);

   // clang-format off
   platform_log("Cache Statistics\n");
   platform_log("-----------------------------------------------------------------------------------------------\n");
   platform_log("page type       |      trunk |     branch |   memtable |     filter |        log |       misc |\n");
   platform_log("----------------|------------|------------|------------|------------|------------|------------|\n");
   platform_log("cache hits      | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
         global_stats.cache_hits[PAGE_TYPE_TRUNK],
         global_stats.cache_hits[PAGE_TYPE_BRANCH],
         global_stats.cache_hits[PAGE_TYPE_MEMTABLE],
         global_stats.cache_hits[PAGE_TYPE_FILTER],
         global_stats.cache_hits[PAGE_TYPE_LOG],
         global_stats.cache_hits[PAGE_TYPE_MISC]);
   platform_log("cache misses    | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
         global_stats.cache_misses[PAGE_TYPE_TRUNK],
         global_stats.cache_misses[PAGE_TYPE_BRANCH],
         global_stats.cache_misses[PAGE_TYPE_MEMTABLE],
         global_stats.cache_misses[PAGE_TYPE_FILTER],
         global_stats.cache_misses[PAGE_TYPE_LOG],
         global_stats.cache_misses[PAGE_TYPE_MISC]);
   platform_log("cache miss time | " FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s | "FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s | "FRACTION_FMT(9, 2)"s | "
                FRACTION_FMT(9, 2)"s |\n",
                FRACTION_ARGS(miss_time[PAGE_TYPE_TRUNK]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_BRANCH]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_MEMTABLE]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_FILTER]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_LOG]),
                FRACTION_ARGS(miss_time[PAGE_TYPE_MISC]));
   platform_log("pages allocated | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
         global_stats.page_allocs[PAGE_TYPE_TRUNK],
         global_stats.page_allocs[PAGE_TYPE_BRANCH],
         global_stats.page_allocs[PAGE_TYPE_MEMTABLE],
         global_stats.page_allocs[PAGE_TYPE_FILTER],
         global_stats.page_allocs[PAGE_TYPE_LOG],
         global_stats.page_allocs[PAGE_TYPE_MISC]);
   platform_log("pages written   | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
         global_stats.page_writes[PAGE_TYPE_TRUNK],
         global_stats.page_writes[PAGE_TYPE_BRANCH],
         global_stats.page_writes[PAGE_TYPE_MEMTABLE],
         global_stats.page_writes[PAGE_TYPE_FILTER],
         global_stats.page_writes[PAGE_TYPE_LOG],
         global_stats.page_writes[PAGE_TYPE_MISC]);
   platform_log("pages read      | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
         global_stats.page_reads[PAGE_TYPE_TRUNK],
         global_stats.page_reads[PAGE_TYPE_BRANCH],
         global_stats.page_reads[PAGE_TYPE_MEMTABLE],
         global_stats.page_reads[PAGE_TYPE_FILTER],
         global_stats.page_reads[PAGE_TYPE_LOG],
         global_stats.page_reads[PAGE_TYPE_MISC]);
   platform_log("avg prefetch pg |  " FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |  "FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |  "FRACTION_FMT(9, 2)" |  "
                FRACTION_FMT(9, 2)" |\n",
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_TRUNK]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_BRANCH]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_MEMTABLE]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_FILTER]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_LOG]),
                FRACTION_ARGS(avg_prefetch_pages[PAGE_TYPE_MISC]));
   platform_log("footprint       | %10lu | %10lu | %10lu | %10lu | %10lu | %10lu |\n",
          global_stats.page_allocs[PAGE_TYPE_TRUNK]
             - global_stats.page_deallocs[PAGE_TYPE_TRUNK],
          global_stats.page_allocs[PAGE_TYPE_BRANCH]
             - global_stats.page_deallocs[PAGE_TYPE_BRANCH],
          global_stats.page_allocs[PAGE_TYPE_MEMTABLE]
             - global_stats.page_deallocs[PAGE_TYPE_MEMTABLE],
          global_stats.page_allocs[PAGE_TYPE_FILTER]
             - global_stats.page_deallocs[PAGE_TYPE_FILTER],
          global_stats.page_allocs[PAGE_TYPE_LOG]
             - global_stats.page_deallocs[PAGE_TYPE_LOG],
          global_stats.page_allocs[PAGE_TYPE_MISC]
             - global_stats.page_deallocs[PAGE_TYPE_MISC]);
   platform_default_log("-----------------------------------------------------------------------------------------------\n");
   platform_log("avg write pgs: "FRACTION_FMT(9,2)"\n",
         FRACTION_ARGS(avg_write_pages));

   uint64 total_space_use_pages = global_stats.page_allocs[PAGE_TYPE_TRUNK]
                                - global_stats.page_deallocs[PAGE_TYPE_TRUNK]
                                + global_stats.page_allocs[PAGE_TYPE_BRANCH]
                                - global_stats.page_deallocs[PAGE_TYPE_BRANCH]
                                + global_stats.page_allocs[PAGE_TYPE_MEMTABLE]
                                - global_stats.page_deallocs[PAGE_TYPE_MEMTABLE]
                                + global_stats.page_allocs[PAGE_TYPE_FILTER]
                                - global_stats.page_deallocs[PAGE_TYPE_FILTER]
                                + global_stats.page_allocs[PAGE_TYPE_LOG]
                                - global_stats.page_deallocs[PAGE_TYPE_LOG]
                                + global_stats.page_allocs[PAGE_TYPE_MISC]
                                - global_stats.page_deallocs[PAGE_TYPE_MISC];
   uint64 total_space_use_bytes = total_space_use_pages * cc->cfg->page_size;
   platform_default_log("\nTotal space use: %lu MiB\n",
         B_TO_MiB(total_space_use_bytes));
   platform_default_log("Total space use (allocator): %lu MiB\n",
         B_TO_MiB(allocator_in_use(cc->al)));
   platform_default_log("Max space use (allocator): %lu MiB\n\n",
         B_TO_MiB(allocator_max_allocated(cc->al)));
   // clang-format on
}

void
clockcache_reset_stats(clockcache *cc)
{
   uint64 i;

   for (i = 0; i < MAX_THREADS; i++) {
      cache_stats *stats = &cc->stats[i];
      ZERO_CONTENTS(stats);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * verification functions for cache_test
 *
 *----------------------------------------------------------------------
 */

uint32
clockcache_count_dirty(clockcache *cc)
{
   uint32 entry_no;
   uint32 dirty_count = 0;
   for (entry_no = 0; entry_no < cc->cfg->page_capacity; entry_no++) {
      if (!clockcache_test_flag(cc, entry_no, CC_CLEAN) &&
          !clockcache_test_flag(cc, entry_no, CC_FREE)) {
         dirty_count++;
      }
   }
   return dirty_count;
}

uint16
clockcache_get_read_ref(clockcache *cc, page_handle *page)
{
   uint32 entry_no = clockcache_page_to_entry_number(cc, page);
   platform_assert(entry_no != CC_UNMAPPED_ENTRY);
   uint16 ref_count = 0;
   for (threadid thr_i = 0; thr_i < CC_RC_WIDTH; thr_i++) {
      ref_count += clockcache_get_ref(cc, entry_no, thr_i);
   }
   return ref_count;
}

bool
clockcache_present(clockcache *cc, page_handle *page)
{
   return clockcache_lookup(cc, page->disk_addr) != CC_UNMAPPED_ENTRY;
}

static void
clockcache_enable_sync_get(clockcache *cc, bool enabled)
{
   cc->per_thread[platform_get_tid()].enable_sync_get = enabled;
}

allocator *
clockcache_allocator(clockcache *cc)
{
   return cc->al;
}

#endif
