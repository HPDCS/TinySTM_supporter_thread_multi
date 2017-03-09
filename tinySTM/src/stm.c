/*
 * File:
 *   stm.c
 * Author(s):
 *   Pascal Felber <pascal.felber@unine.ch>
 *   Patrick Marlier <patrick.marlier@unine.ch>
 * Description:
 *   STM functions.
 *
 * Copyright (c) 2007-2011.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>
#include <sched.h>

#include "stm.h"

#include "atomic.h"
#include "gc.h"

#ifdef HYBRID_ASF
# include "asf/asf-highlevel.h"
/* Abort status */
# define ASF_RETRY                       (0)
# define ASF_FORCE_SOFTWARE              (1)
# define ASF_RETRY_IRREVOCABLE           (2)
/* Number of hybrid tx aborts before to switch to pure software transaction */
# define ASF_ABORT_THRESHOLD             (16)
/* Force lock shift to 0, otherwise a large area will be locked */
# ifdef LOCK_SHIFT_EXTRA 
#  warning LOCK_SHIFT_EXTRA is ignored with HYBRID_ASF
#  undef LOCK_SHIFT_EXTRA
# endif /* LOCK_SHIFT_EXTRA */
# define LOCK_SHIFT_EXTRA                (0)
#endif /* HYBRID_ASF */

/* ################################################################### *
 * DEFINES
 * ################################################################### */


#define COMPILE_TIME_ASSERT(pred)       switch (0) { case 0: case pred: ; }
#if defined(__GNUC__) || defined(__INTEL_COMPILER)
# define likely(x)                      __builtin_expect(!!(x), 1)
# define unlikely(x)                    __builtin_expect(!!(x), 0)
#else /* ! (defined(__GNUC__) || defined(__INTEL_COMPILER)) */
# define likely(x)                      (x)
# define unlikely(x)                    (x)
#endif /* ! (defined(__GNUC__) || defined(__INTEL_COMPILER)) */

/* Designs */
#define WRITE_BACK_ETL                  0
#define WRITE_BACK_CTL                  1
#define WRITE_THROUGH                   2

static const char *design_names[] = {
  /* 0 */ "WRITE-BACK (ETL)",
  /* 1 */ "WRITE-BACK (CTL)",
  /* 2 */ "WRITE-THROUGH"
};

# define DESIGN                         WRITE_BACK_CTL

/* Contention managers */
#define CM_SUICIDE                      0
#define CM_DELAY                        1
#define CM_BACKOFF                      2
#define CM_MODULAR                      3

static const char *cm_names[] = {
  /* 0 */ "SUICIDE",
  /* 1 */ "DELAY",
  /* 2 */ "BACKOFF",
  /* 3 */ "MODULAR"
};

#ifndef CM
# define CM                             CM_SUICIDE
#endif /* ! CM */

#if DESIGN != WRITE_BACK_ETL && CM == CM_MODULAR
# error "MODULAR contention manager can only be used with WB-ETL design"
#endif /* DESIGN != WRITE_BACK_ETL && CM == CM_MODULAR */

#if defined(CONFLICT_TRACKING) && ! defined(EPOCH_GC)
# error "CONFLICT_TRACKING requires EPOCH_GC"
#endif /* defined(CONFLICT_TRACKING) && ! defined(EPOCH_GC) */

#if CM == CM_MODULAR && ! defined(EPOCH_GC)
# error "MODULAR contention manager requires EPOCH_GC"
#endif /* CM == CM_MODULAR && ! defined(EPOCH_GC) */

#if defined(READ_LOCKED_DATA) && CM != CM_MODULAR
# error "READ_LOCKED_DATA can only be used with MODULAR contention manager"
#endif /* defined(READ_LOCKED_DATA) && CM != CM_MODULAR */

#if defined(EPOCH_GC) && defined(SIGNAL_HANDLER)
# error "SIGNAL_HANDLER can only be used without EPOCH_GC"
#endif /* defined(EPOCH_GC) && defined(SIGNAL_HANDLER) */

#if defined(HYBRID_ASF) && CM != CM_SUICIDE
# error "HYBRID_ASF can only be used with SUICIDE contention manager"
#endif /* defined(HYBRID_ASF) && CM != CM_SUICIDE */

#ifdef EXPLICIT_TX_PARAMETER
# define TX_RETURN                      return tx
# define TX_GET                         /* Nothing */
#else /* ! EXPLICIT_TX_PARAMETER */
# define TX_RETURN                      /* Nothing */
# define TX_GET                         stm_tx_t *tx = stm_get_tx()
#endif /* ! EXPLICIT_TX_PARAMETER */

#ifdef DEBUG2
# ifndef DEBUG
#  define DEBUG
# endif /* ! DEBUG */
#endif /* DEBUG2 */

#ifdef DEBUG
/* Note: stdio is thread-safe */
# define IO_FLUSH                       fflush(NULL)
# define PRINT_DEBUG(...)               printf(__VA_ARGS__); fflush(NULL)
#else /* ! DEBUG */
# define IO_FLUSH
# define PRINT_DEBUG(...)
#endif /* ! DEBUG */

#ifdef DEBUG2
# define PRINT_DEBUG2(...)              PRINT_DEBUG(__VA_ARGS__)
#else /* ! DEBUG2 */
# define PRINT_DEBUG2(...)
#endif /* ! DEBUG2 */

#ifndef RW_SET_SIZE
# define RW_SET_SIZE                    4096                /* Initial size of read/write sets */
#endif /* ! RW_SET_SIZE */

#ifndef LOCK_ARRAY_LOG_SIZE
# define LOCK_ARRAY_LOG_SIZE            20                  /* Size of lock array: 2^20 = 1M */
#endif /* LOCK_ARRAY_LOG_SIZE */

#ifndef LOCK_SHIFT_EXTRA
# define LOCK_SHIFT_EXTRA               2                   /* 2 extra shift */
#endif /* LOCK_SHIFT_EXTRA */

#if CM == CM_BACKOFF
# ifndef MIN_BACKOFF
#  define MIN_BACKOFF                   (1UL << 2)
# endif /* MIN_BACKOFF */
# ifndef MAX_BACKOFF
#  define MAX_BACKOFF                   (1UL << 31)
# endif /* MAX_BACKOFF */
#endif /* CM == CM_BACKOFF */

#define NO_SIGNAL_HANDLER               "NO_SIGNAL_HANDLER"

#define XSTR(s)                         STR(s)
#define STR(s)                          #s

/* ################################################################### *
 * TYPES
 * ################################################################### */

enum {                                  /* Transaction status */
  TX_IDLE = 0,
  TX_ACTIVE = 1,                        /* Lowest bit indicates activity */
  TX_COMMITTED = (1 << 1),
  TX_ABORTED = (2 << 1),
  TX_COMMITTING = (1 << 1) | TX_ACTIVE,
  TX_ABORTING = (2 << 1) | TX_ACTIVE,
  TX_KILLED = (3 << 1) | TX_ACTIVE,
  TX_IRREVOCABLE = 0x08 | TX_ACTIVE     /* Fourth bit indicates irrevocability */
};
#define STATUS_BITS                     4
#define STATUS_MASK                     ((1 << STATUS_BITS) - 1)

# define SET_STATUS(s, v)               ((s) = (v))
# define UPDATE_STATUS(s, v)            ((s) = (v))
# define GET_STATUS(s)                  ((s))

#define IS_ACTIVE(s)                    ((GET_STATUS(s) & 0x01) == TX_ACTIVE) 

typedef struct r_entry {                /* Read set entry */
  volatile stm_word_t version;                   /* Version read */
  volatile stm_word_t * volatile lock;            /* Pointer to lock (for fast access) */
} r_entry_t;

typedef struct r_set {                  /* Read set */
  r_entry_t *entries;                   /* Array of entries */
  volatile int nb_entries;                       /* Number of entries */
  int size;                             /* Size of array */
} r_set_t;

typedef struct w_entry {                /* Write set entry */
  union {                               /* For padding... */
    struct {
      volatile stm_word_t *addr;        /* Address written */
      stm_word_t value;                 /* New (write-back) or old (write-through) value */
      stm_word_t mask;                  /* Write mask */
      stm_word_t version;               /* Version overwritten */
      volatile stm_word_t *lock;        /* Pointer to lock (for fast access) */
#if CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
      struct stm_tx *tx;                /* Transaction owning the write set */
#endif /* CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */
#if DESIGN == WRITE_BACK_ETL
      struct w_entry *next;             /* Next address covered by same lock (if any) */
#else /* DESIGN != WRITE_BACK_ETL */
      int no_drop;                      /* Should we drop lock upon abort? */
#endif /* DESIGN != WRITE_BACK_ETL */
    };
    stm_word_t padding[8];              /* Padding (multiple of a cache line) */
    /* TODO Check if padding is really usefull? */
  };
} w_entry_t;

typedef struct w_set {                  /* Write set */
  w_entry_t *entries;                   /* Array of entries */
  int nb_entries;                       /* Number of entries */
  int size;                             /* Size of array */
#if DESIGN == WRITE_BACK_ETL
  int has_writes;                       /* Has the write set any real write (vs. visible reads) */
#elif DESIGN == WRITE_BACK_CTL
  int nb_acquired;                      /* Number of locks acquired */
# ifdef USE_BLOOM_FILTER
  stm_word_t bloom;                     /* Same Bloom filter as in TL2 */
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */
} w_set_t;

typedef struct cb_entry {               /* Callback entry */
  void (*f)(TXPARAMS void *);           /* Function */
  void *arg;                            /* Argument to be passed to function */
} cb_entry_t;

#ifndef MAX_SPECIFIC
# define MAX_SPECIFIC                   16
#endif /* MAX_SPECIFIC */

typedef struct stm_tx {                 /* Transaction descriptor */
  sigjmp_buf env;                       /* Environment for setjmp/longjmp (must be first field!) */
  stm_tx_attr_t attr;                   /* Transaction attributes (user-specified) */
  volatile stm_word_t status;           /* Transaction status */
  stm_word_t start;                     /* Start timestamp */
  volatile stm_word_t end;                       /* End timestamp (validity range) */
  r_set_t r_set;                        /* Read set */
  w_set_t w_set;                        /* Write set */
  unsigned int ro:1;                    /* Is this execution read-only? */
  unsigned int can_extend:1;            /* Can this transaction be extended? */

#ifdef IRREVOCABLE_ENABLED
  unsigned int irrevocable:4;           /* Is this execution irrevocable? */
#endif /* IRREVOCABLE_ENABLED */
#ifdef HYBRID_ASF
  unsigned int software:1;              /* Is the transaction mode pure software? */
#endif /* HYBRID_ASF */
  int nesting;                          /* Nesting level */
  void *data[MAX_SPECIFIC];             /* Transaction-specific data (fixed-size array for better speed) */
  struct stm_tx *next;                  /* For keeping track of all transactional threads */
#ifdef CONFLICT_TRACKING
  pthread_t thread_id;                  /* Thread identifier (immutable) */
#endif /* CONFLICT_TRACKING */
#if CM == CM_DELAY || CM == CM_MODULAR
  volatile stm_word_t *c_lock;          /* Pointer to contented lock (cause of abort) */
#endif /* CM == CM_DELAY || CM == CM_MODULAR */
#if CM == CM_BACKOFF
  unsigned long backoff;                /* Maximum backoff duration */
  unsigned long seed;                   /* RNG seed */
#endif /* CM == CM_BACKOFF */
#if CM == CM_MODULAR || defined(INTERNAL_STATS) || defined(HYBRID_ASF)
  unsigned long retries;                /* Number of consecutive aborts (retries) */
#endif /* CM == CM_MODULAR || defined(INTERNAL_STATS) || defined(HYBRID_ASF) */
#ifdef INTERNAL_STATS
  unsigned long aborts;                 /* Total number of aborts (cumulative) */
  unsigned long aborts_1;               /* Total number of transactions that abort once or more (cumulative) */
  unsigned long aborts_2;               /* Total number of transactions that abort twice or more (cumulative) */
  unsigned long aborts_ro;              /* Aborts due to wrong read-only specification (cumulative) */
  unsigned long aborts_locked_read;     /* Aborts due to trying to read when locked (cumulative) */
  unsigned long aborts_locked_write;    /* Aborts due to trying to write when locked (cumulative) */
  unsigned long aborts_validate_read;   /* Aborts due to failed validation upon read (cumulative) */
  unsigned long aborts_validate_write;  /* Aborts due to failed validation upon write (cumulative) */
  unsigned long aborts_validate_commit; /* Aborts due to failed validation upon commit (cumulative) */
  unsigned long aborts_invalid_memory;  /* Aborts due to invalid memory access (cumulative) */
# ifdef READ_LOCKED_DATA
  unsigned long locked_reads_ok;        /* Successful reads of previous value */
  unsigned long locked_reads_failed;    /* Failed reads of previous value */
# endif /* READ_LOCKED_DATA */
  unsigned long max_retries;            /* Maximum number of consecutive aborts (retries) */
#endif /* INTERNAL_STATS */
#ifdef SUPPORTER_THREAD
  volatile int current_run_checked;
  volatile stm_word_t new_start_timestamp;
  int aborts_supporter_validate_read;
  int error;
  int extended;
  int total_prepares;
  int total_aborts;
  int aborted;
  volatile int should_abort;
  volatile int running_transaction;
  volatile int current_thread_terminated;

#ifdef SUPPORTER_THREAD_TIMERS
  stm_time_t first_start_tx_time;
  stm_time_t last_start_tx_time;
  stm_time_t last_commit_tx_time;
  stm_time_t total_no_tx_time;
  stm_time_t total_tx_wasted_time;
  stm_time_t total_tx_time;
#endif /* ! SUPPORTER_THREAD_TIMERS */

#endif /* ! SUPPORTER_THREAD */
} stm_tx_t;

#ifdef SUPPORTER_THREAD
  typedef struct _run_supporter_thread_data_t {
	  int base_thread_id;
	  int supported_threads;
  } run_supporter_thread_data_t;
//statistics
int aborts_supporter_validate_read=0;
int error=0;
int extended=0;
int total_aborts=0;
int total_prepares=0;
#endif /* ! SUPPORTER_THREAD */

#ifdef SUPPORTER_THREAD_TIMERS
stm_time_t total_no_tx_time;
stm_time_t total_tx_wasted_time;
stm_time_t total_tx_time;
#endif /* ! SUPPORTER_THREAD_TIMERS */

static int nb_specific = 0;             /* Number of specific slots used (<= MAX_SPECIFIC) */

static int initialized = 0;             /* Has the library been initialized? */

static pthread_mutex_t quiesce_mutex;   /* Mutex to support quiescence */
static pthread_cond_t quiesce_cond;     /* Condition variable to support quiescence */
static volatile stm_word_t quiesce;     /* Prevent threads from entering transactions upon quiescence */
static volatile stm_word_t threads_nb;  /* Number of active threads */
static stm_tx_t *threads;               /* Head of linked list of threads */

static stm_tx_attr_t default_attributes = { 0, 0, 0, 0, 0 };

#ifdef IRREVOCABLE_ENABLED
// TODO put this value in a cacheline
static volatile stm_word_t irrevocable = 0;
#endif /* IRREVOCABLE_ENABLED */

/*
 * Transaction nesting is supported in a minimalist way (flat nesting):
 * - When a transaction is started in the context of another
 *   transaction, we simply increment a nesting counter but do not
 *   actually start a new transaction.
 * - The environment to be used for setjmp/longjmp is only returned when
 *   no transaction is active so that it is not overwritten by nested
 *   transactions. This allows for composability as the caller does not
 *   need to know whether it executes inside another transaction.
 * - The commit of a nested transaction simply decrements the nesting
 *   counter. Only the commit of the top-level transaction will actually
 *   carry through updates to shared memory.
 * - An abort of a nested transaction will rollback the top-level
 *   transaction and reset the nesting counter. The call to longjmp will
 *   restart execution before the top-level transaction.
 * Using nested transactions without setjmp/longjmp is not recommended
 * as one would need to explicitly jump back outside of the top-level
 * transaction upon abort of a nested transaction. This breaks
 * composability.
 */

/*
 * Reading from the previous version of locked addresses is implemented
 * by peeking into the write set of the transaction that owns the
 * lock. Each transaction has a unique identifier, updated even upon
 * retry. A special "commit" bit of this identifier is set upon commit,
 * right before writing the values from the redo log to shared memory. A
 * transaction can read a locked address if the identifier of the owner
 * does not change between before and after reading the value and
 * version, and it does not have the commit bit set.
 */

/* ################################################################### *
 * CALLBACKS
 * ################################################################### */

#define MAX_CB                          16

/* Declare as static arrays (vs. lists) to improve cache locality */
static cb_entry_t init_cb[MAX_CB];      /* Init thread callbacks */
static cb_entry_t exit_cb[MAX_CB];      /* Exit thread callbacks */
static cb_entry_t start_cb[MAX_CB];     /* Start callbacks */
static cb_entry_t precommit_cb[MAX_CB]; /* Commit callbacks */
static cb_entry_t commit_cb[MAX_CB];    /* Commit callbacks */
static cb_entry_t abort_cb[MAX_CB];     /* Abort callbacks */

static int nb_init_cb = 0;
static int nb_exit_cb = 0;
static int nb_start_cb = 0;
static int nb_precommit_cb = 0;
static int nb_commit_cb = 0;
static int nb_abort_cb = 0;

#ifdef CONFLICT_TRACKING
static void (*conflict_cb)(stm_tx_t *, stm_tx_t *) = NULL;
#endif /* CONFLICT_TRACKING */

/* ################################################################### *
 * THREAD-LOCAL
 * ################################################################### */

#ifdef TLS
static __thread stm_tx_t* thread_tx = NULL;
#else /* ! TLS */
static pthread_key_t thread_tx;
#endif /* ! TLS */

/* ################################################################### *
 * LOCKS
 * ################################################################### */

/*
 * A lock is a unsigned int of the size of a pointer.
 * The LSB is the lock bit. If it is set, this means:
 * - At least some covered memory addresses is being written.
 * - Write-back (ETL): all bits of the lock apart from the lock bit form
 *   a pointer that points to the write log entry holding the new
 *   value. Multiple values covered by the same log entry and orginized
 *   in a linked list in the write log.
 * - Write-through and write-back (CTL): all bits of the lock apart from
 *   the lock bit form a pointer that points to the transaction
 *   descriptor containing the write-set.
 * If the lock bit is not set, then:
 * - All covered memory addresses contain consistent values.
 * - Write-back (ETL and CTL): all bits of the lock besides the lock bit
 *   contain a version number (timestamp).
 * - Write-through: all bits of the lock besides the lock bit contain a
 *   version number.
 *   - The high order bits contain the commit time.
 *   - The low order bits contain an incarnation number (incremented
 *     upon abort while writing the covered memory addresses).
 * When visible reads are enabled, two bits are used as read and write
 * locks. A read-locked address can be read by an invisible reader.
 */

#if CM == CM_MODULAR
# define OWNED_BITS                     2                   /* 2 bits */
# define WRITE_MASK                     0x01                /* 1 bit */
# define READ_MASK                      0x02                /* 1 bit */
# define OWNED_MASK                     (WRITE_MASK | READ_MASK)
#else /* CM != CM_MODULAR */
# define OWNED_BITS                     1                   /* 1 bit */
# define WRITE_MASK                     0x01                /* 1 bit */
# define OWNED_MASK                     (WRITE_MASK)
#endif /* CM != CM_MODULAR */
# define LOCK_BITS                      (OWNED_BITS)
#define MAX_THREADS                     8192                /* Upper bound (large enough) */
#define VERSION_MAX                     ((~(stm_word_t)0 >> LOCK_BITS) - MAX_THREADS)

#define LOCK_GET_OWNED(l)               (l & OWNED_MASK)
#define LOCK_GET_WRITE(l)               (l & WRITE_MASK)
#define LOCK_SET_ADDR_WRITE(a)          (a | WRITE_MASK)    /* WRITE bit set */
#define LOCK_GET_ADDR(l)                (l & ~(stm_word_t)OWNED_MASK)
#if CM == CM_MODULAR
# define LOCK_GET_READ(l)               (l & READ_MASK)
# define LOCK_SET_ADDR_READ(a)          (a | READ_MASK)     /* READ bit set */
# define LOCK_UPGRADE(l)                (l | WRITE_MASK)
#endif /* CM == CM_MODULAR */
#if DESIGN == WRITE_THROUGH
# define LOCK_GET_TIMESTAMP(l)          (l >> (1 + INCARNATION_BITS))
# define LOCK_SET_TIMESTAMP(t)          (t << (1 + INCARNATION_BITS))
# define LOCK_GET_INCARNATION(l)        ((l & INCARNATION_MASK) >> 1)
# define LOCK_SET_INCARNATION(i)        (i << 1)            /* OWNED bit not set */
# define LOCK_UPD_INCARNATION(l, i)     ((l & ~(stm_word_t)(INCARNATION_MASK | OWNED_MASK)) | LOCK_SET_INCARNATION(i))
#else /* DESIGN != WRITE_THROUGH */
# define LOCK_GET_TIMESTAMP(l)          (l >> OWNED_BITS)   /* Logical shift (unsigned) */
# define LOCK_SET_TIMESTAMP(t)          (t << OWNED_BITS)   /* OWNED bits not set */
#endif /* DESIGN != WRITE_THROUGH */
#define LOCK_UNIT                       (~(stm_word_t)0)

/*
 * We use the very same hash functions as TL2 for degenerate Bloom
 * filters on 32 bits.
 */
#ifdef USE_BLOOM_FILTER
# define FILTER_HASH(a)                 (((stm_word_t)a >> 2) ^ ((stm_word_t)a >> 5))
# define FILTER_BITS(a)                 (1 << (FILTER_HASH(a) & 0x1F))
#endif /* USE_BLOOM_FILTER */

/*
 * We use an array of locks and hash the address to find the location of the lock.
 * We try to avoid collisions as much as possible (two addresses covered by the same lock).
 */
#define LOCK_ARRAY_SIZE                 (1 << LOCK_ARRAY_LOG_SIZE)
#define LOCK_MASK                       (LOCK_ARRAY_SIZE - 1)
#define LOCK_SHIFT                      (((sizeof(stm_word_t) == 4) ? 2 : 3) + LOCK_SHIFT_EXTRA)
#define LOCK_IDX(a)                     (((stm_word_t)(a) >> LOCK_SHIFT) & LOCK_MASK)
#ifdef LOCK_IDX_SWAP
# if LOCK_ARRAY_LOG_SIZE < 16
#  error "LOCK_IDX_SWAP requires LOCK_ARRAY_LOG_SIZE to be at least 16"
# endif /* LOCK_ARRAY_LOG_SIZE < 16 */
# define GET_LOCK(a)                    (locks + lock_idx_swap(LOCK_IDX(a)))
#else /* ! LOCK_IDX_SWAP */
# define GET_LOCK(a)                    (locks + LOCK_IDX(a))
#endif /* ! LOCK_IDX_SWAP */

static volatile stm_word_t locks[LOCK_ARRAY_SIZE];

/* ################################################################### *
 * CLOCK
 * ################################################################### */

#ifdef CLOCK_IN_CACHE_LINE
/* At least twice a cache line (512 bytes to be on the safe side) */
static volatile stm_word_t gclock[1024 / sizeof(stm_word_t)];
# define CLOCK                          (gclock[512 / sizeof(stm_word_t)])
#else /* ! CLOCK_IN_CACHE_LINE */
static volatile stm_word_t gclock;
# define CLOCK                          (gclock)
#endif /* ! CLOCK_IN_CACHE_LINE */

#define GET_CLOCK                       (ATOMIC_LOAD_ACQ(&CLOCK))
#define FETCH_INC_CLOCK                 (ATOMIC_FETCH_INC_FULL(&CLOCK))

#ifdef SUPPORTER_THREAD

/* ################################################################### *
 * SUPPORTER THREAD
 * ################################################################### */

pthread_spinlock_t stm_tx_pointers_spinlock;

static volatile stm_tx_t* stm_tx_pointers[MAX_THREADS];

#endif /* ! SUPPORTER_THREAD */



/* ################################################################### *
 * STATIC
 * ################################################################### */

/*
 * Returns the transaction descriptor for the CURRENT thread.
 */
static inline stm_tx_t *stm_get_tx()
{
#ifdef TLS
  return thread_tx;
#else /* ! TLS */
  return (stm_tx_t *)pthread_getspecific(thread_tx);
#endif /* ! TLS */
}

#ifdef LOCK_IDX_SWAP
/*
 * Compute index in lock table (swap bytes to avoid consecutive addresses to have neighboring locks).
 */
static inline unsigned int lock_idx_swap(unsigned int idx) {
  return (idx & ~(unsigned int)0xFFFF) | ((idx & 0x00FF) << 8) | ((idx & 0xFF00) >> 8);
}
#endif /* LOCK_IDX_SWAP */

/*
 * Initialize quiescence support.
 */
static inline void stm_quiesce_init()
{
  PRINT_DEBUG("==> stm_quiesce_init()\n");

  if (pthread_mutex_init(&quiesce_mutex, NULL) != 0) {
    fprintf(stderr, "Error creating mutex\n");
    exit(1);
  }
  if (pthread_cond_init(&quiesce_cond, NULL) != 0) {
    fprintf(stderr, "Error creating condition variable\n");
    exit(1);
  }
  quiesce = 0;
  threads_nb = 0;
  threads = NULL;
}

/*
 * Clean up quiescence support.
 */
static inline void stm_quiesce_exit()
{
  PRINT_DEBUG("==> stm_quiesce_exit()\n");

  pthread_cond_destroy(&quiesce_cond);
  pthread_mutex_destroy(&quiesce_mutex);
}

/*
 * Called by each thread upon initialization for quiescence support.
 */
static inline void stm_quiesce_enter_thread(stm_tx_t *tx)
{
  PRINT_DEBUG("==> stm_quiesce_enter_thread(%p)\n", tx);

  pthread_mutex_lock(&quiesce_mutex);
  /* Add new descriptor at head of list */
  tx->next = threads;
  threads = tx;
  threads_nb++;
  pthread_mutex_unlock(&quiesce_mutex);
}

/*
 * Called by each thread upon exit for quiescence support.
 */
static inline void stm_quiesce_exit_thread(stm_tx_t *tx)
{
  stm_tx_t *t, *p;

  PRINT_DEBUG("==> stm_quiesce_exit_thread(%p)\n", tx);

  /* Can only be called if non-active */
  assert(!IS_ACTIVE(tx->status));

  pthread_mutex_lock(&quiesce_mutex);
  /* Remove descriptor from list */
  p = NULL;
  t = threads;
  while (t != tx) {
    assert(t != NULL);
    p = t;
    t = t->next;
  }
  if (p == NULL)
    threads = t->next;
  else
    p->next = t->next;
  threads_nb--;
  if (quiesce) {
    /* Wake up someone in case other threads are waiting for us */
    pthread_cond_signal(&quiesce_cond);
  }
  pthread_mutex_unlock(&quiesce_mutex);
}

/*
 * Wait for all transactions to be block on a barrier.
 */
static inline void stm_quiesce_barrier(stm_tx_t *tx, void (*f)(void *), void *arg)
{
  PRINT_DEBUG("==> stm_quiesce_barrier()\n");

  /* Can only be called if non-active */
  assert(tx == NULL || !IS_ACTIVE(tx->status));

  pthread_mutex_lock(&quiesce_mutex);
  /* Wait for all other transactions to block on barrier */
  threads_nb--;
  if (quiesce == 0) {
    /* We are first on the barrier */
    quiesce = 1;
  }
  while (quiesce) {
    if (threads_nb == 0) {
      /* Everybody is blocked */
      if (f != NULL)
        f(arg);
      /* Release transactional threads */
      quiesce = 0;
      pthread_cond_broadcast(&quiesce_cond);
    } else {
      /* Wait for other transactions to stop */
      pthread_cond_wait(&quiesce_cond, &quiesce_mutex);
    }
  }
  threads_nb++;
  pthread_mutex_unlock(&quiesce_mutex);
}

/*
 * Wait for all transactions to be be out of their current transaction.
 */
static inline int stm_quiesce(stm_tx_t *tx, int block)
{
  stm_tx_t *t;

  PRINT_DEBUG("==> stm_quiesce(%p)\n", tx);

  /* TODO ASF doesn't support pthread_mutex_* since it may require syscall. */
  if (IS_ACTIVE(tx->status)) {
    /* Only one active transaction can quiesce at a time, others must abort */
    if (pthread_mutex_trylock(&quiesce_mutex) != 0)
      return 1;
  } else {
    /* We can safely block because we are inactive */
    pthread_mutex_lock(&quiesce_mutex);
  }
  /* We own the lock at this point */
  if (block)
    ATOMIC_STORE_REL(&quiesce, 2);
  /* Make sure we read latest status data */
  ATOMIC_MB_FULL;
  /* Not optimal as we check transaction sequentially and might miss some inactivity states */
  for (t = threads; t != NULL; t = t->next) {
    if (t == tx)
      continue;
    /* Wait for all other transactions to become inactive */

    while (IS_ACTIVE(t->status))
      ;

  }
  if (!block)
    pthread_mutex_unlock(&quiesce_mutex);
  return 0;
}

/*
 * Check if transaction must block.
 */
static inline int stm_check_quiesce(stm_tx_t *tx)
{
  stm_word_t s;

  /* Must be called upon start (while already active but before acquiring any lock) */
  assert(IS_ACTIVE(tx->status));

#ifdef IRREVOCABLE_ENABLED
  if ((tx->irrevocable & 0x08) != 0) {
    /* Serial irrevocable mode: we are executing alone */
    return 0;
  }
#endif
  ATOMIC_MB_FULL;
  if (ATOMIC_LOAD_ACQ(&quiesce) == 2) {
    s = ATOMIC_LOAD(&tx->status);
    SET_STATUS(tx->status, TX_IDLE);
    while (ATOMIC_LOAD_ACQ(&quiesce) == 2) {
#ifdef WAIT_YIELD
      sched_yield();
#endif /* WAIT_YIELD */
    }
    SET_STATUS(tx->status, GET_STATUS(s));
    return 1;
  }
  return 0;
}

/*
 * Release threads blocked after quiescence.
 */
static inline void stm_quiesce_release(stm_tx_t *tx)
{
  ATOMIC_STORE_REL(&quiesce, 0);
  pthread_mutex_unlock(&quiesce_mutex);
}

/*
 * Reset clock and timestamps
 */
static inline void rollover_clock(void *arg)
{
  PRINT_DEBUG("==> rollover_clock()\n");

  /* Reset clock */
  CLOCK = 0;
  /* Reset timestamps */
  memset((void *)locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));
# ifdef EPOCH_GC
  /* Reset GC */
  gc_reset();
# endif /* EPOCH_GC */
}

/*
 * Check if stripe has been read previously.
 */
static inline r_entry_t *stm_has_read(stm_tx_t *tx, volatile stm_word_t *lock)
{
  r_entry_t *r;
  int i;

  PRINT_DEBUG("==> stm_has_read(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, lock);

  /* Look for read */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    if (r->lock == lock) {
      /* Return first match*/
      return r;
    }
  }
  return NULL;
}

#if DESIGN == WRITE_BACK_CTL
/*
 * Check if address has been written previously.
 */
static inline w_entry_t *stm_has_written(stm_tx_t *tx, volatile stm_word_t *addr)
{
  w_entry_t *w;
  int i;
# ifdef USE_BLOOM_FILTER
  stm_word_t mask;
# endif /* USE_BLOOM_FILTER */

  PRINT_DEBUG("==> stm_has_written(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);

# ifdef USE_BLOOM_FILTER
  mask = FILTER_BITS(addr);
  if ((tx->w_set.bloom & mask) != mask)
    return NULL;
# endif /* USE_BLOOM_FILTER */

  /* Look for write */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    if (w->addr == addr) {
      return w;
    }
  }
  return NULL;
}
#endif /* DESIGN == WRITE_BACK_CTL */

/*
 * (Re)allocate read set entries.
 */
static inline void stm_allocate_rs_entries(stm_tx_t *tx, int extend)
{
  PRINT_DEBUG("==> stm_allocate_rs_entries(%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, extend);

  if (extend) {
    /* Extend read set */
    tx->r_set.size *= 2;
    if ((tx->r_set.entries = (r_entry_t *)realloc(tx->r_set.entries, tx->r_set.size * sizeof(r_entry_t))) == NULL) {
      perror("realloc read set");
      exit(1);
    }
  } else {
    /* Allocate read set */
    if ((tx->r_set.entries = (r_entry_t *)malloc(tx->r_set.size * sizeof(r_entry_t))) == NULL) {
      perror("malloc read set");
      exit(1);
    }
  }
}

/*
 * (Re)allocate write set entries.
 */
static inline void stm_allocate_ws_entries(stm_tx_t *tx, int extend)
{
#if CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
  int i, first = (extend ? tx->w_set.size : 0);
#endif /* CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */

  PRINT_DEBUG("==> stm_allocate_ws_entries(%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, extend);

  if (extend) {
    /* Extend write set */
#if DESIGN == WRITE_BACK_ETL
    int j;
    w_entry_t *ows, *nws;
    /* Allocate new write set */
    ows = tx->w_set.entries;
    if ((nws = (w_entry_t *)malloc(tx->w_set.size * 2 * sizeof(w_entry_t))) == NULL) {
      perror("malloc write set");
      exit(1);
    }
    /* Copy write set */
    memcpy(nws, ows, tx->w_set.size * sizeof(w_entry_t));
    /* Update pointers and locks */
    for (j = 0; j < tx->w_set.nb_entries; j++) {
      if (ows[j].next != NULL)
        nws[j].next = nws + (ows[j].next - ows);
    }
    for (j = 0; j < tx->w_set.nb_entries; j++) {
      if (ows[j].lock == GET_LOCK(ows[j].addr)) 
        ATOMIC_STORE_REL(ows[j].lock, LOCK_SET_ADDR_WRITE((stm_word_t)&nws[j]));
    }
    tx->w_set.entries = nws;
    tx->w_set.size *= 2;
# ifdef EPOCH_GC
    gc_free(ows, tx->start);
# else /* ! EPOCH_GC */
    free(ows);
# endif /* ! EPOCH_GC */
#else /* DESIGN != WRITE_BACK_ETL */
    tx->w_set.size *= 2;
    if ((tx->w_set.entries = (w_entry_t *)realloc(tx->w_set.entries, tx->w_set.size * sizeof(w_entry_t))) == NULL) {
      perror("realloc write set");
      exit(1);
    }
#endif /* DESIGN != WRITE_BACK_ETL */
  } else {
    /* Allocate write set */
    if ((tx->w_set.entries = (w_entry_t *)malloc(tx->w_set.size * sizeof(w_entry_t))) == NULL) {
      perror("malloc write set");
      exit(1);
    }
  }

#if CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH)
  /* Initialize fields */
  for (i = first; i < tx->w_set.size; i++)
    tx->w_set.entries[i].tx = tx;
#endif /* CM == CM_MODULAR || (defined(CONFLICT_TRACKING) && DESIGN != WRITE_THROUGH) */
}

#ifdef SUPPORTER_THREAD
/*
 * Validate read set (check if all read addresses are still valid now).
 */
static inline int _stm_validate(stm_tx_t *tx)
{

	r_entry_t *r;
	int i;
	stm_word_t l;

	PRINT_DEBUG("==> stm_validate(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

	/* Validate reads */
	i = tx->r_set.nb_entries;
	r = tx->r_set.entries;
	for (; i > 0; i--, r++) {
		if (!tx->running_transaction) return 1;
		/* Read lock */
		l = ATOMIC_LOAD(r->lock);
		/* Unlocked and still the same version? */
		//if (LOCK_GET_OWNED(l)) {
		if (0) {

#ifdef CONFLICT_TRACKING
			if (conflict_cb != NULL && l != LOCK_UNIT) {
				/* Call conflict callback */
				stm_tx_t *other = ((w_entry_t *)LOCK_GET_ADDR(l))->tx;
				conflict_cb(tx, other);
			}
#endif /* CONFLICT_TRACKING */
			return 0;
		} else {
			//************************************
			//int v=LOCK_GET_TIMESTAMP(l);
			//int k=r->version;
			//if(tot-i==0) {
				//printf("\n\ttot %i posto %i dataitem % i version % i - timestamp %i clock %i",tot,tot-i,l, k,v, GET_CLOCK);
				//fflush(stdout);
			//}
			//************************************
			if (LOCK_GET_TIMESTAMP(l) > tx->end) {
				/* Other version: cannot validate */
				return 0;
			}
			/* Same version: OK */
		}
	}
	//printf("\n--------------------------------------");
	return 1;
}
#endif

static inline void print_readset(stm_tx_t *tx) {

	r_entry_t *r;
	int i;
	stm_word_t l;

	i = tx->r_set.nb_entries;
	int tot=i;
	r = tx->r_set.entries;
	for (; i > 0; i--, r++) {
		/* Read lock */
		l = ATOMIC_LOAD(r->lock);
		int v = LOCK_GET_TIMESTAMP(l);
		int k = r->version;
		printf("\n\t\t\t\ttot %i position, %i order %i, version % i, timestamp %i",tot, tot-i,l, k,v);
		fflush(stdout);
	}
}

/*
 * Validate read set (check if all read addresses are still valid now).
 */
static inline int stm_validate(stm_tx_t *tx)
{
  r_entry_t *r;
  int i;
  stm_word_t l;

  PRINT_DEBUG("==> stm_validate(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Validate reads */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    /* Read lock */
    l = ATOMIC_LOAD(r->lock);
    /* Unlocked and still the same version? */
    if (LOCK_GET_OWNED(l)) {
      /* Do we own the lock? */
#if DESIGN == WRITE_THROUGH
      if ((stm_tx_t *)LOCK_GET_ADDR(l) != tx)
#else /* DESIGN != WRITE_THROUGH */
      w_entry_t *w = (w_entry_t *)LOCK_GET_ADDR(l);
      /* Simply check if address falls inside our write set (avoids non-faulting load) */
      if (!(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries))
#endif /* DESIGN != WRITE_THROUGH */
      {
        /* Locked by another transaction: cannot validate */
#ifdef CONFLICT_TRACKING
        if (conflict_cb != NULL && l != LOCK_UNIT) {
          /* Call conflict callback */
# if DESIGN == WRITE_THROUGH
          stm_tx_t *other = (stm_tx_t *)LOCK_GET_ADDR(l);
# else /* DESIGN != WRITE_THROUGH */
          stm_tx_t *other = ((w_entry_t *)LOCK_GET_ADDR(l))->tx;
# endif /* DESIGN != WRITE_THROUGH */
          conflict_cb(tx, other);
        }
#endif /* CONFLICT_TRACKING */
        return 0;
      }
      /* We own the lock: OK */
#if DESIGN == WRITE_BACK_CTL
      if (w->version != r->version) {
        /* Other version: cannot validate */
        return 0;
      }
#endif /* DESIGN == WRITE_BACK_CTL */
    } else {
      if (LOCK_GET_TIMESTAMP(l) != r->version) {
        /* Other version: cannot validate */
        return 0;
      }
      /* Same version: OK */
    }
  }
  return 1;
}

/*
 * Extend snapshot range.
 */
static inline int stm_extend(stm_tx_t *tx)
{
  stm_word_t now;

  PRINT_DEBUG("==> stm_extend(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Get current time */
  now = GET_CLOCK;
  if (now >= VERSION_MAX) {
    /* Clock overflow */
    return 0;
  }
  /* Try to validate read set */
  if (stm_validate(tx)) {
    /* It works: we can extend until now */
    tx->end = now;
    return 1;
  }
  return 0;
}

/*
 * Initialize the transaction descriptor before start or restart.
 */
static inline void stm_prepare(stm_tx_t *tx)
{

 start:
  /* Start timestamp */
  tx->start = tx->end = GET_CLOCK; /* OPT: Could be delayed until first read/write */

  /* Allow extensions */
  tx->can_extend = 1;
  if (tx->start >= VERSION_MAX) {
    /* Block all transactions and reset clock */
    stm_quiesce_barrier(tx, rollover_clock, NULL);
    goto start;
  }

  /* Read/write set */

  tx->w_set.nb_acquired = 0;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom = 0;
# endif /* USE_BLOOM_FILTER */

  tx->w_set.nb_entries = 0;
  tx->r_set.nb_entries = 0;

#ifdef EPOCH_GC
  gc_set_epoch(tx->start);
#endif /* EPOCH_GC */

#ifdef IRREVOCABLE_ENABLED
  if (tx->irrevocable != 0) {
    assert(!IS_ACTIVE(tx->status));
    stm_set_irrevocable(TXARGS -1);
    UPDATE_STATUS(tx->status, TX_IRREVOCABLE);
  } else
    UPDATE_STATUS(tx->status, TX_ACTIVE);
#else /* ! IRREVOCABLE_ENABLED */
  /* Set status */
  UPDATE_STATUS(tx->status, TX_ACTIVE);
#endif /* ! IRREVOCABLE_ENABLED */

  stm_check_quiesce(tx);

#ifdef SUPPORTER_THREAD
  tx->current_run_checked=0;
  tx->total_prepares++;

  //printf("\n\t\t\treset -  %i", GET_CLOCK);
  tx->should_abort=0;
  tx->running_transaction=1;
#endif /* ! SUPPORTER_THREAD */

}

/*
 * Rollback transaction.
 */
static inline void stm_rollback(stm_tx_t *tx, int reason)
{
  w_entry_t *w;



  PRINT_DEBUG("==> stm_rollback(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

#ifdef SUPPORTER_THREAD
  tx->total_aborts++;
  tx->aborted=1;
  tx->running_transaction=0;
#endif /* ! SUPPORTER_THREAD */

  assert(IS_ACTIVE(tx->status));

  if (tx->w_set.nb_acquired > 0) {
    w = tx->w_set.entries + tx->w_set.nb_entries;
    do {
      w--;
      if (!w->no_drop) {
        if (--tx->w_set.nb_acquired == 0) {
          /* Make sure that all lock releases become visible to other threads */
          ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(w->version));
        } else {
          ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(w->version));
        }
      }
    } while (tx->w_set.nb_acquired > 0);
  }


#if CM == CM_MODULAR || defined(INTERNAL_STATS)
  tx->retries++;
#endif /* CM == CM_MODULAR || defined(INTERNAL_STATS) */
#ifdef INTERNAL_STATS
  tx->aborts++;
  if (tx->retries == 1)
    tx->aborts_1++;
  else if (tx->retries == 2)
    tx->aborts_2++;
  if (tx->max_retries < tx->retries)
    tx->max_retries = tx->retries;
#endif /* INTERNAL_STATS */

  /* Set status to ABORTED */
  SET_STATUS(tx->status, TX_ABORTED);

  /* Reset nesting level */
  tx->nesting = 1;

  /* Callbacks */
  if (nb_abort_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_abort_cb; cb++)
      abort_cb[cb].f(TXARGS abort_cb[cb].arg);
  }


  /* TODO: what is the expected behavior of STM_ABORT_EXPLICIT? */
  /* Don't prepare a new transaction if no retry. */
  if (tx->attr.no_retry || (reason & STM_ABORT_EXPLICIT) != 0) {
    tx->nesting = 0;
    return;
  }

  /* Reset field to restart transaction */
  stm_prepare(tx);



#ifdef SUPPORTER_THREAD_TIMERS
  tx->last_start_tx_time = STM_TIMER_READ();
#endif SUPPORTER_THREAD_TIMERS

  /* Jump back to transaction start */
  /* FIXME: not proper at all. it has to be changed. The value must be the one specified by the function. */
#if defined(TM_DTMC)
  /* Restore stack */
  tanger_stm_restore_stack();
  /* TODO if irrevocable + DTMC, can run uninstrumented */
#endif /* defined(TM_DTMC) */
#if defined(TM_DTMC) || defined(TM_GCC) || defined (TM_INTEL)
  siglongjmp(tx->env, 0x09); /* ABI 0x09 = runInstrumented + restoreLiveVariable */
#else /* !defined(TM_DTMC) && !defined(TM_GCC) && !defined (TM_INTEL) */
  siglongjmp(tx->env, reason);
#endif /* !defined(TM_DTMC) && !defined(TM_GCC) && !defined (TM_INTEL) */
}

/*
 * Load a word-sized value (invisible read).
 */
static inline stm_word_t stm_read_invisible(stm_tx_t *tx, volatile stm_word_t *addr)
{
  volatile stm_word_t *lock;
  stm_word_t l, l2, value, version;
  r_entry_t *r;
  w_entry_t *written = NULL;


  PRINT_DEBUG2("==> stm_read_invisible(t=%p[%lu-%lu],a=%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);

#if CM != CM_MODULAR
  assert(IS_ACTIVE(tx->status));
#endif /* CM != CM_MODULAR */

#if DESIGN == WRITE_BACK_CTL
  /* Did we previously write the same address? */
  written = stm_has_written(tx, addr);
  if (written != NULL) {
    /* Yes: get value from write set if possible */
    if (written->mask == ~(stm_word_t)0) {
      value = written->value;
      /* No need to add to read set */
      return value;
    }
  }
#endif /* DESIGN == WRITE_BACK_CTL */

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Note: we could check for duplicate reads and get value from read set */

  /* Read lock, value, lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_WRITE(l)) {
    /* Locked */
    if (l == LOCK_UNIT) {
      /* Data modified by a unit store: should not last long => retry */
      goto restart;
    }
    /* Do we own the lock? */

    /* Spin while locked (should not last long) */
    goto restart;

  } else {
    /* Not locked */
    value = ATOMIC_LOAD_ACQ(addr);
    l2 = ATOMIC_LOAD_ACQ(lock);
    if (l != l2) {
      l = l2;
      goto restart_no_load;
    }
#ifdef IRREVOCABLE_ENABLED
    /* In irrevocable mode, no need check timestamp nor add entry to read set */
    if (tx->irrevocable)
      return value;
#endif /* IRREVOCABLE_ENABLED */
    /* Check timestamp */

    version = LOCK_GET_TIMESTAMP(l);

    /* Valid version? */
    if (version > tx->end) {
#ifdef IRREVOCABLE_ENABLED
      assert(!tx->irrevocable);
#endif /* IRREVOCABLE_ENABLED */
      /* No: try to extend first (except for read-only transactions: no read set) */
      if (tx->ro || !tx->can_extend || !stm_extend(tx)) {
        /* Not much we can do: abort */

#ifdef INTERNAL_STATS
        tx->aborts_validate_read++;
#endif /* INTERNAL_STATS */
        stm_rollback(tx, STM_ABORT_VAL_READ);
        return 0;
      }
      /* Verify that version has not been overwritten (read value has not
       * yet been added to read set and may have not been checked during
       * extend) */
      l = ATOMIC_LOAD_ACQ(lock);
      if (l != l2) {
        l = l2;
        goto restart_no_load;
      }
      /* Worked: we now have a good version (version <= tx->end) */
    }

  }
  /* We have a good version: add to read set (update transactions) and return value */

#if DESIGN == WRITE_BACK_CTL
  /* Did we previously write the same address? */
  if (written != NULL) {
    value = (value & ~written->mask) | (written->value & written->mask);
    /* Must still add to read set */
  }
#endif /* DESIGN == WRITE_BACK_CTL */
#ifdef READ_LOCKED_DATA
 add_to_read_set:
#endif /* READ_LOCKED_DATA */
  if (!tx->ro) {
#ifdef NO_DUPLICATES_IN_RW_SETS
    if (stm_has_read(tx, lock) != NULL)
      return value;
#endif /* NO_DUPLICATES_IN_RW_SETS */
    /* Add address and version to read set */
    if (tx->r_set.nb_entries == tx->r_set.size)
      stm_allocate_rs_entries(tx, 1);
#ifdef SUPPORTER_THREAD
    r = &tx->r_set.entries[tx->r_set.nb_entries];
    r->version = version;
    r->lock = lock;
    tx->r_set.nb_entries++;
#else
  r = &tx->r_set.entries[tx->r_set.nb_entries++];
  r->version = version;
  r->lock = lock;
#endif /* ! SUPPORTER_THREAD */

	//printf("\n\t\t\t\t\t\t\tdataitem % i version % i - timestamp %i",l, r->version, LOCK_GET_TIMESTAMP(l));
	//fflush(stdout);
  }
  return value;
}

/*
 * Store a word-sized value (return write set entry or NULL).
 */
static inline w_entry_t *stm_write(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  volatile stm_word_t *lock;
  stm_word_t l, version;
  w_entry_t *w;

  PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,d=%p-%lu,m=0x%lx)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  assert(IS_ACTIVE(tx->status));


  if (tx->ro) {
    /* Disable read-only and abort */
    tx->attr.read_only = 0;
#ifdef INTERNAL_STATS
    tx->aborts_ro++;
#endif /* INTERNAL_STATS */
    stm_rollback(tx, STM_ABORT_RO_WRITE);
    return NULL;
  }

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Try to acquire lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_OWNED(l)) {
    /* Locked */
    if (l == LOCK_UNIT) {
      /* Data modified by a unit store: should not last long => retry */
      goto restart;
    }
    /* Do we own the lock? */

    /* Spin while locked (should not last long) */
    goto restart;

  }
  /* Not locked */
#if DESIGN == WRITE_BACK_CTL
  w = stm_has_written(tx, addr);
  if (w != NULL) {
    w->value = (w->value & ~mask) | (value & mask);
    w->mask |= mask;
    return w;
  }
#endif /* DESIGN == WRITE_BACK_CTL */
  /* Handle write after reads (before CAS) */
  version = LOCK_GET_TIMESTAMP(l);
#ifdef IRREVOCABLE_ENABLED
  /* In irrevocable mode, no need to revalidate */
  if (tx->irrevocable)
    goto acquire_no_check;
#endif /* IRREVOCABLE_ENABLED */
 acquire:
  if (version > tx->end) {
    /* We might have read an older version previously */
    if (!tx->can_extend || stm_has_read(tx, lock) != NULL) {
      /* Read version must be older (otherwise, tx->end >= version) */
      /* Not much we can do: abort */

#ifdef INTERNAL_STATS
      tx->aborts_validate_write++;
#endif /* INTERNAL_STATS */
      stm_rollback(tx, STM_ABORT_VAL_WRITE);
      return NULL;
    }
  }
  /* Acquire lock (ETL) */
#ifdef IRREVOCABLE_ENABLED
 acquire_no_check:
#endif /* IRREVOCABLE_ENABLED */
  /* We own the lock here (ETL) */
do_write:
  /* Add address to write set */
#if DESIGN != WRITE_BACK_ETL
  if (tx->w_set.nb_entries == tx->w_set.size)
    stm_allocate_ws_entries(tx, 1);
  w = &tx->w_set.entries[tx->w_set.nb_entries++];
#endif /* DESIGN != WRITE_BACK_ETL */
  w->addr = addr;
  w->mask = mask;
  w->lock = lock;
  if (mask == 0) {
    /* Do not write anything */
#ifndef NDEBUG
    w->value = 0;
#endif /* ! NDEBUG */
  } else {
    /* Remember new value */
    w->value = value;
  }
# ifndef NDEBUG
  w->version = version;
# endif
  w->no_drop = 1;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom |= FILTER_BITS(addr) ;
# endif /* USE_BLOOM_FILTER */


#ifdef IRREVOCABLE_ENABLED
  if (!tx->irrevocable && ATOMIC_LOAD_ACQ(&irrevocable)) {
    stm_rollback(tx, STM_ABORT_IRREVOCABLE);
    return NULL;
  }
#endif /* IRREVOCABLE_ENABLED */

  return w;
}

/*
 * Store a word-sized value in a unit transaction.
 */
static inline int stm_unit_write(volatile stm_word_t *addr, stm_word_t value, stm_word_t mask, stm_word_t *timestamp)
{
  volatile stm_word_t *lock;
  stm_word_t l;

  PRINT_DEBUG2("==> stm_unit_write(a=%p,d=%p-%lu,m=0x%lx)\n",
               addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Try to acquire lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
  if (LOCK_GET_OWNED(l)) {
    /* Locked: wait until lock is free */
#ifdef WAIT_YIELD
    sched_yield();
#endif /* WAIT_YIELD */
    goto restart;
  }
  /* Not locked */
  if (timestamp != NULL && LOCK_GET_TIMESTAMP(l) > *timestamp) {
    /* Return current timestamp */
    *timestamp = LOCK_GET_TIMESTAMP(l);
    return 0;
  }
  /* TODO: would need to store thread ID to be able to kill it (for wait freedom) */
  if (ATOMIC_CAS_FULL(lock, l, LOCK_UNIT) == 0)
    goto restart;
  ATOMIC_STORE(addr, value);
  /* Update timestamp with newer value (may exceed VERSION_MAX by up to MAX_THREADS) */
  l = FETCH_INC_CLOCK + 1;
  if (timestamp != NULL)
    *timestamp = l;
  /* Make sure that lock release becomes visible */
  ATOMIC_STORE_REL(lock, LOCK_SET_TIMESTAMP(l));
  if (l >= VERSION_MAX) {
    /* Block all transactions and reset clock (current thread is not in active transaction) */
    stm_quiesce_barrier(NULL, rollover_clock, NULL);
  }
  return 1;
}

#ifdef SIGNAL_HANDLER
/*
 * Catch signal (to emulate non-faulting load).
 */
static void signal_catcher(int sig)
{
  sigset_t block_signal;
  stm_tx_t *tx = stm_get_tx();

  /* A fault might only occur upon a load concurrent with a free (read-after-free) */
  PRINT_DEBUG("Caught signal: %d\n", sig);

  /* TODO: TX_KILLED should be also allowed */
  if (tx == NULL || tx->attr.no_retry || GET_STATUS(tx->status) != TX_ACTIVE) {
    /* There is not much we can do: execution will restart at faulty load */
    fprintf(stderr, "Error: invalid memory accessed and no longjmp destination\n");
    exit(1);
  }

# ifdef INTERNAL_STATS
  tx->aborts_invalid_memory++;
# endif /* INTERNAL_STATS */

  /* Unblock the signal since there is no return to signal handler */
  sigemptyset(&block_signal);
  sigaddset(&block_signal, sig);
  pthread_sigmask(SIG_UNBLOCK, &block_signal, NULL);

  /* Will cause a longjmp */
  stm_rollback(tx, STM_ABORT_SIGNAL);
}
#endif /* SIGNAL_HANDLER */

/* ################################################################### *
 * STM FUNCTIONS
 * ################################################################### */

/*
 * Called once (from main) to initialize STM infrastructure.
 */

#ifdef SUPPORTER_THREAD

static inline void check_should_abort() {
	TX_GET;

	if (tx->should_abort && tx->current_run_checked){
		tx->running_transaction=0;
		tx->aborts_supporter_validate_read++;

		if (stm_validate(tx)) {
			tx->error++;
			//TO_DO
			//printf("\nerror");
			//fflush(stdout);
		}

		//printf("\n\t\t\tshould_abort - aborted: %i ", tx->aborts_supporter_validate_read);
		//fflush(stdout);
        stm_rollback(tx, STM_ABORT_VAL_READ);
	} else {
		//extend tx


		if (tx->end<tx->new_start_timestamp) {
			tx->end=tx->new_start_timestamp;
			tx->extended++;
			//printf("\n\t\textended %i %i ",tx->start, tx->end);
		}


	}
}

void run_supporter_thread(void * data) {

	int i;
	stm_word_t now=0;
	//int supporter_thread_id=pthread_self();
	int main_thread_id=((run_supporter_thread_data_t*) data)->base_thread_id;

	//printf("supporter_thread_id %i created, base thread %i\n", supporter_thread_id, main_thread_id );
	//fflush(stdout);

	stm_tx_t *stm_tx_pointer;
	//move this thread on its CPU-core
	/*
	  cpu_set_t *cpuSetMask=(cpu_set_t*)malloc(sizeof(cpu_set_t));
	  __CPU_ZERO_S(sizeof(cpu_set_t),cpuSetMask);
	  __CPU_SET_S((main_thread_id*2)+1+16, sizeof(cpu_set_t), cpuSetMask);
	  // set thread affinity
	  if (sched_setaffinity(0, sizeof(cpu_set_t), cpuSetMask)!=0) {
	  	printf("\nsched_setaffinity error - errno: %i ",errno);
	  }
	 */

	//int supporter_thread_ratio=((run_supporter_thread_data_t*) data)->supporter_thread_ratio;
   int w=0;
	while(1) {
		w++;
	}

	while(1) {

		for (i=main_thread_id; i<main_thread_id+((run_supporter_thread_data_t*) data)->supported_threads; i++) {

			stm_tx_pointer=stm_tx_pointers[i];
			if (stm_tx_pointer==NULL) continue;
			if (!stm_tx_pointer->running_transaction || stm_tx_pointer->should_abort) continue;
			//printf("\nsupporter thread %i is checking thread %i", supporter_thread_id,  i);
			//fflush(stdout);

			now=CLOCK;
			/*
			if (now<=stm_tx_pointer->end) {
				printf("\nnow = stm_tx_pointer->end = %llu", now);
				continue;
			}
			printf("\nnow: %llu\tstm_tx_pointer->end: %llu", now, stm_tx_pointer->end);
			*/
			stm_tx_pointer->current_run_checked=1;
			//pthread_spin_lock(&test_spinlock);
			//int g=_stm_validate(main_stm_tx);
			//pthread_spin_unlock(&test_spinlock);
			if (_stm_validate(stm_tx_pointer)) {
				stm_tx_pointer->new_start_timestamp = now;
				//printf("\nCan extend: thread_id %lu from %i to %i ",stm_tx_pointer->thread_id, stm_tx_pointer->end,now);
				//fflush(stdout);
			} else {
				//printf("\nsetting should_abort: time: %i ", now);
				stm_tx_pointer->should_abort=1;
				//printf("\set should_abort thread_id: %lu", stm_tx_pointer->thread_id);
				//fflush(stdout);
			}


		}
	}
}

#endif /* ! SUPPORTER_THREAD */

#ifdef SUPPORTER_THREAD
void stm_init(int num_tm_threads, int numSupportedThreads)
#else /* ! SUPPORTER_THREAD */
void stm_init()
#endif
{


#ifdef SIGNAL_HANDLER
  struct sigaction act;
#endif /* SIGNAL_HANDLER */

  PRINT_DEBUG("==> stm_init()\n");

  if (initialized)
    return;

#ifdef SUPPORTER_THREAD
  //supporter_thread_ratio=sp_ratio;
  pthread_spin_init(&stm_tx_pointers_spinlock, 0);

  //pthread_spin_lock(&test_spinlock);
  memset(stm_tx_pointers,NULL, sizeof(stm_tx_t*)*MAX_THREADS);

  //printf("supp_threads %i\n", numSupportedThreads);
  //fflush(stdout);

  //create #supp_threads  supporter threads
  int i;
  for (i=0;i<num_tm_threads;i++) {
	  if (numSupportedThreads!=0 && i%numSupportedThreads==0) {
		  run_supporter_thread_data_t* run_supporter_thread_data = malloc(sizeof(run_supporter_thread_data_t));
		  run_supporter_thread_data->base_thread_id=i;
		  run_supporter_thread_data->supported_threads=numSupportedThreads;
		  pthread_t supporter_thread;
		  pthread_create(&supporter_thread, NULL, (void *) &run_supporter_thread, (void *) run_supporter_thread_data);
	  }
  }

#endif /* ! SUPPORTER_THREAD */

  PRINT_DEBUG("\tsizeof(word)=%d\n", (int)sizeof(stm_word_t));

  PRINT_DEBUG("\tVERSION_MAX=0x%lx\n", (unsigned long)VERSION_MAX);

  COMPILE_TIME_ASSERT(sizeof(stm_word_t) == sizeof(void *));
  COMPILE_TIME_ASSERT(sizeof(stm_word_t) == sizeof(atomic_t));

#ifdef EPOCH_GC
  gc_init(stm_get_clock);
#endif /* EPOCH_GC */

  memset((void *)locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));


  CLOCK = 0;
  stm_quiesce_init();

#ifndef TLS
  if (pthread_key_create(&thread_tx, NULL) != 0) {
    fprintf(stderr, "Error creating thread local\n");
    exit(1);
  }
#endif /* ! TLS */

#ifdef SIGNAL_HANDLER
  if (getenv(NO_SIGNAL_HANDLER) == NULL) {
    /* Catch signals for non-faulting load */
    act.sa_handler = signal_catcher;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGBUS, &act, NULL) < 0 || sigaction(SIGSEGV, &act, NULL) < 0) {
      perror("sigaction");
      exit(1);
    }
  }
#endif /* SIGNAL_HANDLER */
  initialized = 1;
}

/*
 * Called once (from main) to clean up STM infrastructure.
 */
void stm_exit()
{
  PRINT_DEBUG("==> stm_exit()\n");

#ifndef TLS
  pthread_key_delete(thread_tx);
#endif /* ! TLS */
  stm_quiesce_exit();

#ifdef EPOCH_GC
  gc_exit();
#endif /* EPOCH_GC */

#ifdef SUPPORTER_THREAD /* SUPPORTER_THREAD */
 printf("\ttotal supporter aborted: %i error: %i ", aborts_supporter_validate_read,error);
 printf("\textended: %i ", extended);
 printf("\ttotal aborted: %i ", total_aborts);
 printf("\ttotal prepares: %i ", total_prepares);


#ifdef SUPPORTER_THREAD_TIMERS
  printf("\ttotal_no_tx_time %f wasted time %f usefull time %f\n",(float)total_no_tx_time/(float)1000000,(float)total_tx_wasted_time/(float)1000000, (float)total_tx_time/(float)1000000);
#endif /* ! SUPPORTER_THREAD_TIMERS */
#endif /* ! SUPPORTER_THREAD */
}



/*
 * Called by the CURRENT thread to initialize thread-local STM data.
 */
TXTYPE stm_init_thread()
{
  stm_tx_t *tx;

  PRINT_DEBUG("==> stm_init_thread()\n");

  if ((tx = stm_get_tx()) != NULL)
    TX_RETURN;

#ifdef EPOCH_GC
  gc_init_thread();
#endif /* EPOCH_GC */

  /* Allocate descriptor */
  if ((tx = (stm_tx_t *)malloc(sizeof(stm_tx_t))) == NULL) {
    perror("malloc tx");
    exit(1);
  }
  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_IDLE;
  /* Read set */
  tx->r_set.nb_entries = 0;
  tx->r_set.size = RW_SET_SIZE;
  stm_allocate_rs_entries(tx, 0);
  /* Write set */
  tx->w_set.nb_entries = 0;
  tx->w_set.size = RW_SET_SIZE;
#if DESIGN == WRITE_BACK_CTL
  tx->w_set.nb_acquired = 0;
# ifdef USE_BLOOM_FILTER
  tx->w_set.bloom = 0;
# endif /* USE_BLOOM_FILTER */
#endif /* DESIGN == WRITE_BACK_CTL */
  stm_allocate_ws_entries(tx, 0);
  /* Nesting level */
  tx->nesting = 0;
  /* Transaction-specific data */
  memset(tx->data, 0, MAX_SPECIFIC * sizeof(void *));
#ifdef CONFLICT_TRACKING
  /* Thread identifier */
  tx->thread_id = pthread_self();
#endif /* CONFLICT_TRACKING */

#if CM == CM_MODULAR || defined(INTERNAL_STATS) || defined(HYBRID_ASF)
  tx->retries = 0;
#endif /* CM == CM_MODULAR || defined(INTERNAL_STATS) || defined(HYBRID_ASF) */
#ifdef INTERNAL_STATS
  /* Statistics */
  tx->aborts = 0;
  tx->aborts_1 = 0;
  tx->aborts_2 = 0;
  tx->aborts_ro = 0;
  tx->aborts_locked_read = 0;
  tx->aborts_locked_write = 0;
  tx->aborts_validate_read = 0;
  tx->aborts_validate_write = 0;
  tx->aborts_validate_commit = 0;
  tx->aborts_invalid_memory = 0;

# ifdef READ_LOCKED_DATA
  tx->locked_reads_ok = 0;
  tx->locked_reads_failed = 0;
# endif /* READ_LOCKED_DATA */
  tx->max_retries = 0;
#endif /* INTERNAL_STATS */
#ifdef HYBRID_ASF
  tx->software = 0;
#endif /* HYBRID_ASF */
#ifdef IRREVOCABLE_ENABLED
  tx->irrevocable = 0;
#endif /* IRREVOCABLE_ENABLED */
  /* Store as thread-local data */
#ifdef TLS
  thread_tx = tx;
#else /* ! TLS */
  pthread_setspecific(thread_tx, tx);
#endif /* ! TLS */
  stm_quiesce_enter_thread(tx);

#ifdef SUPPORTER_THREAD
  tx->current_thread_terminated=0;
  tx->aborts_supporter_validate_read=0;
  tx->error=0;
  tx->extended=0;
  tx->total_aborts=0;
  tx->total_prepares=0;
  tx->running_transaction=0;
  tx->should_abort=0;
  tx->new_start_timestamp=0;
  tx->current_run_checked=0;

#ifdef SUPPORTER_THREAD_TIMERS
  tx->first_start_tx_time=0;
  tx->last_start_tx_time=0;
  tx->last_commit_tx_time=STM_TIMER_READ();
  tx->total_no_tx_time=0;
  tx->total_tx_wasted_time=0;
  tx->total_tx_time=0;
#endif /* ! SUPPORTER_THREAD */

  // find the first free location and store thread_tx pointer
  pthread_spin_lock(&stm_tx_pointers_spinlock);
  int i=0;
  while (i<MAX_THREADS) {
	  if (stm_tx_pointers[i]==NULL) {
		  stm_tx_pointers[i]=tx;
		  break;
	  }
	  i++;
  }
  pthread_spin_unlock(&stm_tx_pointers_spinlock);

//SPOSTARE LA CREAZIONE DEL THREAD SUPPORTER DENTRO TM_INIT


  //move this thread on its CPU-core
  /*
  cpu_set_t *cpuSetMask=(cpu_set_t*)malloc(sizeof(cpu_set_t));
  __CPU_ZERO_S(sizeof(cpu_set_t),cpuSetMask);
  __CPU_SET_S(i*2+16, sizeof(cpu_set_t), cpuSetMask);
  // set thread affinity
  if (sched_setaffinity(0, sizeof(cpu_set_t), cpuSetMask)!=0) {
  	printf("\nsched_setaffinity error - errno: %i ",errno);
  }
  */

#endif /* ! SUPPORTER_THREAD */

  /* Callbacks */
  if (nb_init_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_init_cb; cb++)
      init_cb[cb].f(TXARGS init_cb[cb].arg);
  }

  TX_RETURN;
}

/*
 * Called by the CURRENT thread to cleanup thread-local STM data.
 */
void stm_exit_thread(TXPARAM)
{
#ifdef EPOCH_GC
  stm_word_t t;
#endif /* EPOCH_GC */
  TX_GET;

  PRINT_DEBUG("==> stm_exit_thread(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

#ifdef SUPPORTER_THREAD /* SUPPORTER_THREAD */
  tx->current_thread_terminated=1;
  // find the first free location and store thread_tx pointer
  pthread_spin_lock(&stm_tx_pointers_spinlock);
  int i;
  while (i<MAX_THREADS) {
	  if (stm_tx_pointers[i]==tx) {
		  stm_tx_pointers[i]=NULL;
		  break;
	  }
	  i++;
  }

   aborts_supporter_validate_read+=tx->aborts_supporter_validate_read;
   error+=tx->error;
   extended+=tx->extended;
   total_aborts+=tx->total_aborts;
   total_prepares+=tx->total_prepares;
#ifdef SUPPORTER_THREAD_TIMERS
   total_no_tx_time+=tx->total_no_tx_time;
   total_tx_wasted_time+=tx->total_tx_wasted_time;
   total_tx_time+=tx->total_tx_time;
#endif /* ! SUPPORTER_THREAD_TIMERS */

   pthread_spin_unlock(&stm_tx_pointers_spinlock);


#endif /* ! SUPPORTER_THREAD */

  /* Callbacks */
  if (nb_exit_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_exit_cb; cb++)
      exit_cb[cb].f(TXARGS exit_cb[cb].arg);
  }

  stm_quiesce_exit_thread(tx);

#ifdef EPOCH_GC
  t = GET_CLOCK;
  gc_free(tx->r_set.entries, t);
  gc_free(tx->w_set.entries, t);
  gc_free(tx, t);
  gc_exit_thread();
#else /* ! EPOCH_GC */
  free(tx->r_set.entries);
  free(tx->w_set.entries);
  free(tx);
#endif /* ! EPOCH_GC */

#ifdef TLS
  thread_tx = NULL;
#else /* ! TLS */
  pthread_setspecific(thread_tx, NULL);
#endif /* ! TLS */

}

/*
 * Called by the CURRENT thread to start a transaction.
 */
sigjmp_buf *stm_start(TXPARAMS stm_tx_attr_t *attr)
{

  TX_GET;

#ifdef SUPPORTER_THREAD
#ifdef SUPPORTER_THREAD_TIMERS
  tx->first_start_tx_time = STM_TIMER_READ();
  tx->total_no_tx_time+=tx->first_start_tx_time-tx->last_commit_tx_time;
#endif /* SUPPORTER_THREAD_TIMERS */

  tx->aborted=0;
#endif /* ! SUPPORTER_THREAD */

  PRINT_DEBUG("==> stm_start(%p)\n", tx);

  /* Increment nesting level */
  if (tx->nesting++ > 0)
    return NULL;

  /* Attributes */
  tx->attr = (attr == NULL ? default_attributes : *attr);
  tx->ro = tx->attr.read_only; /* TODO ro is a duplicate attribute */

  /* Initialize transaction descriptor */


  stm_prepare(tx);

  /* TODO: verify that mod_* deal with the new specification of stm_start (called only once per tx) */
  /* Callbacks */
  if (nb_start_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_start_cb; cb++)
      start_cb[cb].f(TXARGS start_cb[cb].arg);
  }

  return &tx->env;
}

/*
 * Called by the CURRENT thread to commit a transaction.
 */
int stm_commit(TXPARAM)
{

#ifdef SUPPORTER_THREAD
  check_should_abort();
#endif /* ! SUPPORTER_THREAD */

	//pthread_spin_lock(&test_spinlock);
	//pthread_spin_unlock(&test_spinlock);

  w_entry_t *w;
  stm_word_t t;
  int i;
#if DESIGN == WRITE_BACK_CTL
  stm_word_t l, value;
#endif /* DESIGN == WRITE_BACK_CTL */
  TX_GET;


  //printf("\n\t\t\tclock before: %i ", GET_CLOCK);

#ifdef SUPPORTER_THREAD
  tx->running_transaction=0;
	if (tx->should_abort && tx->current_run_checked) {
		//print_readset(tx);
		printf("\n\t\t\tshould_abort: %i ", GET_CLOCK);
		fflush(stdout);
	}
#endif /* ! SUPPORTER_THREAD */


  PRINT_DEBUG("==> stm_commit(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Decrement nesting level */
  if (--tx->nesting > 0){
    return 1;
  }

  /* Callbacks */
  if (nb_precommit_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_precommit_cb; cb++)
      precommit_cb[cb].f(TXARGS precommit_cb[cb].arg);
  }

  assert(IS_ACTIVE(tx->status));

  /* A read-only transaction can commit immediately */
  if (tx->w_set.nb_entries == 0)
    goto end;

  /* Update transaction */
#if DESIGN == WRITE_BACK_CTL
# ifdef IRREVOCABLE_ENABLED
  /* Verify already if there is an irrevocable transaction before acquiring locks */
  if(!tx->irrevocable && ATOMIC_LOAD(&irrevocable)) {
    stm_rollback(tx, STM_ABORT_IRREVOCABLE);
    return 0;
  }
# endif /* IRREVOCABLE_ENABLED */
  /* Acquire locks (in reverse order) */
  w = tx->w_set.entries + tx->w_set.nb_entries;
  do {
    w--;
    /* Try to acquire lock */
 restart:
    l = ATOMIC_LOAD(w->lock);
    if (LOCK_GET_OWNED(l)) {
      /* Do we already own the lock? */
      if (tx->w_set.entries <= (w_entry_t *)LOCK_GET_ADDR(l) && (w_entry_t *)LOCK_GET_ADDR(l) < tx->w_set.entries + tx->w_set.nb_entries) {
        /* Yes: ignore */
        continue;
      }
      /* Conflict: CM kicks in */
      /* Abort self */
# ifdef INTERNAL_STATS
      tx->aborts_locked_write++;
# endif /* INTERNAL_STATS */
      stm_rollback(tx, STM_ABORT_WW_CONFLICT);
      return 0;
    }
    if (ATOMIC_CAS_FULL(w->lock, l, LOCK_SET_ADDR_WRITE((stm_word_t)w)) == 0)
      goto restart;
    /* We own the lock here */
    w->no_drop = 0;
    /* Store version for validation of read set */
    w->version = LOCK_GET_TIMESTAMP(l);
    tx->w_set.nb_acquired++;
  } while (w > tx->w_set.entries);
#endif /* DESIGN == WRITE_BACK_CTL */

#ifdef IRREVOCABLE_ENABLED
  /* Verify if there is an irrevocable transaction once all locks have been acquired */
# ifdef IRREVOCABLE_IMPROVED
  /* FIXME: it is bogus. the status should be changed to idle otherwise stm_quiesce will not progress */
  if (!tx->irrevocable) {
    do {
      t = ATOMIC_LOAD(&irrevocable);
      /* If the irrevocable transaction have encountered an acquired lock, abort */
      if (t == 2) {
        stm_rollback(tx, STM_ABORT_IRREVOCABLE);
        return 0;
      }
    } while (t);
  }
# else /* ! IRREVOCABLE_IMPROVED */
  if (!tx->irrevocable && ATOMIC_LOAD(&irrevocable)) {
    stm_rollback(tx, STM_ABORT_IRREVOCABLE);
    return 0;
  }
# endif /* ! IRREVOCABLE_IMPROVED */
#endif /* IRREVOCABLE_ENABLED */ 
  /* Get commit timestamp (may exceed VERSION_MAX by up to MAX_THREADS) */
  t = FETCH_INC_CLOCK + 1;
 // printf("\n\t\t\tclock after: %i ", GET_CLOCK);
#ifdef IRREVOCABLE_ENABLED
  if (tx->irrevocable)
    goto release_locks;
#endif /* IRREVOCABLE_ENABLED */

  /* Try to validate (only if a concurrent transaction has committed since tx->start) */
  if (tx->start != t - 1 && !stm_validate(tx)) {
    /* Cannot commit */
#ifdef INTERNAL_STATS
    tx->aborts_validate_commit++;
#endif /* INTERNAL_STATS */
    stm_rollback(tx, STM_ABORT_VALIDATE);
    return 0;
  }

#ifdef IRREVOCABLE_ENABLED
  release_locks:
#endif /* IRREVOCABLE_ENABLED */



  /* Install new versions, drop locks and set new timestamp */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    /* TODO stm_release */
    //if (w->addr == NULL)
      //continue;
    if (w->mask == ~(stm_word_t)0) {
      ATOMIC_STORE(w->addr, w->value);
    } else if (w->mask != 0) {
      value = (ATOMIC_LOAD(w->addr) & ~w->mask) | (w->value & w->mask);
      ATOMIC_STORE(w->addr, value);
    }
    /* Only drop lock for last covered address in write set (cannot be "no drop") */
    if (!w->no_drop)
      ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));
  }


 end:

#ifdef SUPPORTER_THREAD_TIMERS

 	 tx->last_commit_tx_time=STM_TIMER_READ();
 	 if (tx->aborted>0) {
 		 tx->total_tx_wasted_time+=tx->last_start_tx_time-tx->first_start_tx_time;
 		 tx->total_tx_time+=STM_TIMER_READ()-tx->last_start_tx_time;
 	 } else {
 		 tx->total_tx_time+=STM_TIMER_READ()-tx->first_start_tx_time;
 	 }

#endif /* SUPPORTER_THREAD_TIMERS */

#if CM == CM_MODULAR || defined(INTERNAL_STATS)
  tx->retries = 0;
#endif /* CM == CM_MODULAR || defined(INTERNAL_STATS) */


#ifdef HYBRID_ASF
  /* Reset to Hybrid mode */
  tx->software = 0;
#endif /* HYBRID_ASF */

#ifdef IRREVOCABLE_ENABLED
  if (tx->irrevocable) {
    ATOMIC_STORE(&irrevocable, 0);
    if ((tx->irrevocable & 0x08) != 0)
      stm_quiesce_release(tx);
    tx->irrevocable = 0;
  }
#endif /* IRREVOCABLE_ENABLED */

  /* Set status to COMMITTED */
  SET_STATUS(tx->status, TX_COMMITTED);

  /* Callbacks */
  if (nb_commit_cb != 0) {
    int cb;
    for (cb = 0; cb < nb_commit_cb; cb++)
      commit_cb[cb].f(TXARGS commit_cb[cb].arg);
  }

  return 1;
}

/*
 * Called by the CURRENT thread to abort a transaction.
 */
void stm_abort(TXPARAMS int reason)
{
  TX_GET;
  stm_rollback(tx, reason | STM_ABORT_EXPLICIT);
}

/*
 * Called by the CURRENT thread to load a word-sized value.
 */
stm_word_t stm_load(TXPARAMS volatile stm_word_t *addr)
{
  TX_GET;
#ifdef SUPPORTER_THREAD
  check_should_abort();
#endif /* ! SUPPORTER_THREAD */

#ifdef IRREVOCABLE_ENABLED
  if (unlikely(((tx->irrevocable & 0x08) != 0))) {
    /* Serial irrevocable mode: direct access to memory */
    return ATOMIC_LOAD(addr);
  }
#endif /* IRREVOCABLE_ENABLED */

  return stm_read_invisible(tx, addr);
}

/*
 * Called by the CURRENT thread to store a word-sized value.
 */
void stm_store(TXPARAMS volatile stm_word_t *addr, stm_word_t value)
{
  TX_GET;

#ifdef SUPPORTER_THREAD
  //check_should_abort();
#endif /* ! SUPPORTER_THREAD */

#ifdef IRREVOCABLE_ENABLED
  if (unlikely(((tx->irrevocable & 0x08) != 0))) {
    /* Serial irrevocable mode: direct access to memory */
    ATOMIC_STORE(addr, value);
    return;
  }
#endif /* IRREVOCABLE_ENABLED */
  stm_write(tx, addr, value, ~(stm_word_t)0);
}

/*
 * Called by the CURRENT thread to store part of a word-sized value.
 */
void stm_store2(TXPARAMS volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  TX_GET;

#ifdef IRREVOCABLE_ENABLED
  if (unlikely(((tx->irrevocable & 0x08) != 0))) {
    /* Serial irrevocable mode: direct access to memory */
    if (mask == ~(stm_word_t)0)
      ATOMIC_STORE(addr, value);
    else
      ATOMIC_STORE(addr, (ATOMIC_LOAD(addr) & ~mask) | (value & mask));
    return;
  }
#endif /* IRREVOCABLE_ENABLED */
  stm_write(tx, addr, value, mask);
}

/*
 * Called by the CURRENT thread to inquire about the status of a transaction.
 */
int stm_active(TXPARAM)
{
  TX_GET;
  assert (tx != NULL);
  return IS_ACTIVE(tx->status);
}

/*
 * Called by the CURRENT thread to inquire about the status of a transaction.
 */
int stm_aborted(TXPARAM)
{
  TX_GET;
  assert (tx != NULL);
  return (GET_STATUS(tx->status) == TX_ABORTED);
}

# ifdef IRREVOCABLE_ENABLED
/*
 * Called by the CURRENT thread to inquire about the status of a transaction.
 */
int stm_irrevocable(TXPARAM)
{
  TX_GET;
  assert (tx != NULL);
  return ((tx->irrevocable & 0x07) == 3);
}
# endif /* IRREVOCABLE_ENABLED */

/*
 * Called by the CURRENT thread to obtain an environment for setjmp/longjmp.
 */
sigjmp_buf *stm_get_env(TXPARAM)
{
  TX_GET;
  assert (tx != NULL);
  /* Only return environment for top-level transaction */
  return tx->nesting == 0 ? &tx->env : NULL;
}

/*
 * Get transaction attributes.
 */
stm_tx_attr_t *stm_get_attributes(TXPARAM)
{
  TX_GET;

  return &tx->attr;
}

/*
 * Get transaction attributes from a specifc transaction.
 */
stm_tx_attr_t *stm_get_attributes_tx(struct stm_tx *tx)
{ 
  return &tx->attr;
}

/*
 * Return statistics about a thread/transaction.
 */
int stm_get_stats(TXPARAMS const char *name, void *val)
{
  TX_GET;
  assert (tx != NULL);

  if (strcmp("read_set_size", name) == 0) {
    *(unsigned int *)val = tx->r_set.size;
    return 1;
  }
  if (strcmp("write_set_size", name) == 0) {
    *(unsigned int *)val = tx->w_set.size;
    return 1;
  }
  if (strcmp("read_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->r_set.nb_entries;
    return 1;
  }
  if (strcmp("write_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->w_set.nb_entries;
    return 1;
  }
  if (strcmp("read_only", name) == 0) {
    *(unsigned int *)val = tx->ro;
    return 1;
  }
#ifdef INTERNAL_STATS
  if (strcmp("nb_aborts", name) == 0) {
    *(unsigned long *)val = tx->aborts;
    return 1;
  }
  if (strcmp("nb_aborts_1", name) == 0) {
    *(unsigned long *)val = tx->aborts_1;
    return 1;
  }
  if (strcmp("nb_aborts_2", name) == 0) {
    *(unsigned long *)val = tx->aborts_2;
    return 1;
  }
  if (strcmp("nb_aborts_ro", name) == 0) {
    *(unsigned long *)val = tx->aborts_ro;
    return 1;
  }
  if (strcmp("nb_aborts_locked_read", name) == 0) {
    *(unsigned long *)val = tx->aborts_locked_read;
    return 1;
  }
  if (strcmp("nb_aborts_locked_write", name) == 0) {
    *(unsigned long *)val = tx->aborts_locked_write;
    return 1;
  }
  if (strcmp("nb_aborts_validate_read", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_read;
    return 1;
  }
  if (strcmp("nb_aborts_validate_write", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_write;
    return 1;
  }
  if (strcmp("nb_aborts_validate_commit", name) == 0) {
    *(unsigned long *)val = tx->aborts_validate_commit;
    return 1;
  }
  if (strcmp("nb_aborts_invalid_memory", name) == 0) {
    *(unsigned long *)val = tx->aborts_invalid_memory;
    return 1;
  }

# ifdef READ_LOCKED_DATA
  if (strcmp("locked_reads_ok", name) == 0) {
    *(unsigned long *)val = tx->locked_reads_ok;
    return 1;
  }
  if (strcmp("locked_reads_failed", name) == 0) {
    *(unsigned long *)val = tx->locked_reads_failed;
    return 1;
  }
# endif /* READ_LOCKED_DATA */
  if (strcmp("max_retries", name) == 0) {
    *(unsigned long *)val = tx->max_retries;
    return 1;
  }
#endif /* INTERNAL_STATS */
  return 0;
}

/*
 * Return STM parameters.
 */
int stm_get_parameter(const char *name, void *val)
{
  if (strcmp("contention_manager", name) == 0) {
    *(const char **)val = cm_names[CM];
    return 1;
  }
  if (strcmp("design", name) == 0) {
    *(const char **)val = design_names[DESIGN];
    return 1;
  }
  if (strcmp("initial_rw_set_size", name) == 0) {
    *(int *)val = RW_SET_SIZE;
    return 1;
  }

#ifdef COMPILE_FLAGS
  if (strcmp("compile_flags", name) == 0) {
    *(const char **)val = XSTR(COMPILE_FLAGS);
    return 1;
  }
#endif /* COMPILE_FLAGS */
  return 0;
}

/*
 * Set STM parameters.
 */
int stm_set_parameter(const char *name, void *val)
{
  return 0;
}

/*
 * Create transaction-specific data (return -1 on error).
 */
int stm_create_specific()
{
  if (nb_specific >= MAX_SPECIFIC) {
    fprintf(stderr, "Error: maximum number of specific slots reached\n");
    return -1;
  }
  return nb_specific++;
}

/*
 * Store transaction-specific data.
 */
void stm_set_specific(TXPARAMS int key, void *data)
{
  TX_GET;

  assert (key >= 0 && key < nb_specific);
  tx->data[key] = data;
}

/*
 * Fetch transaction-specific data.
 */
void *stm_get_specific(TXPARAMS int key)
{
  TX_GET;

  assert (key >= 0 && key < nb_specific);
  return tx->data[key];
}

/*
 * Register callbacks for an external module (must be called before creating transactions).
 */
int stm_register(void (*on_thread_init)(TXPARAMS void *arg),
                 void (*on_thread_exit)(TXPARAMS void *arg),
                 void (*on_start)(TXPARAMS void *arg),
                 void (*on_precommit)(TXPARAMS void *arg),
                 void (*on_commit)(TXPARAMS void *arg),
                 void (*on_abort)(TXPARAMS void *arg),
                 void *arg)
{
  if ((on_thread_init != NULL && nb_init_cb >= MAX_CB) ||
      (on_thread_exit != NULL && nb_exit_cb >= MAX_CB) ||
      (on_start != NULL && nb_start_cb >= MAX_CB) ||
      (on_precommit != NULL && nb_precommit_cb >= MAX_CB) ||
      (on_commit != NULL && nb_commit_cb >= MAX_CB) ||
      (on_abort != NULL && nb_abort_cb >= MAX_CB)) {
    fprintf(stderr, "Error: maximum number of modules reached\n");
    return 0;
  }
  /* New callback */
  if (on_thread_init != NULL) {
    init_cb[nb_init_cb].f = on_thread_init;
    init_cb[nb_init_cb++].arg = arg;
  }
  /* Delete callback */
  if (on_thread_exit != NULL) {
    exit_cb[nb_exit_cb].f = on_thread_exit;
    exit_cb[nb_exit_cb++].arg = arg;
  }
  /* Start callback */
  if (on_start != NULL) {
    start_cb[nb_start_cb].f = on_start;
    start_cb[nb_start_cb++].arg = arg;
  }
  /* Pre-commit callback */
  if (on_precommit != NULL) {
    precommit_cb[nb_precommit_cb].f = on_precommit;
    precommit_cb[nb_precommit_cb++].arg = arg;
  }
  /* Commit callback */
  if (on_commit != NULL) {
    commit_cb[nb_commit_cb].f = on_commit;
    commit_cb[nb_commit_cb++].arg = arg;
  }
  /* Abort callback */
  if (on_abort != NULL) {
    abort_cb[nb_abort_cb].f = on_abort;
    abort_cb[nb_abort_cb++].arg = arg;
  }

  return 1;
}

#if 0
void stm_release(TXPARAMS volatile stm_word_t *addr)
{
// TODO to test 
  w_entry_t *w;
  volatile stm_word_t *lock;
#if DESIGN == WRITE_THROUGH
  int must_release = 1;
  stm_word_t t, i;
#elif DESIGN == WRITE_BACK_ETL
  stm_word_t l;
#endif
  TX_GET;

#if CM == CM_MODULAR
  if (GET_STATUS(tx->status) == TX_KILLED) {
    stm_rollback(tx, STM_ABORT_KILLED);
    return;
  }
#else /* CM != CM_MODULAR */
  assert(GET_STATUS(tx->status) == TX_ACTIVE);
#endif /* CM != CM_MODULAR */

  /* Get reference to lock */
  lock = GET_LOCK(addr);

#if DESIGN == WRITE_THROUGH
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    /* If the lock covers this address */
    if (GET_LOCK(w->addr) == lock) {
      if (w->addr == addr) {
        /* Restore value */
        ATOMIC_STORE(w->addr, w->value);
        w->addr = NULL;
        if (w->no_drop)
          return;
	break;
      } else if (w->no_drop == 1) {
        w->no_drop = 0;
        must_release = 0;
	break;
      }
    }
  }
  if (must_release) {
    /* Release lock */
    /* Incarnation numbers allow readers to detect dirty reads */
    i = LOCK_GET_INCARNATION(w->version) + 1;
    if (i > INCARNATION_MAX) {
      /* Simple approach: write new version (might trigger unnecessary aborts) */
      if (t == 0) {
        t = FETCH_INC_CLOCK + 1;
        if (t >= VERSION_MAX) {
# ifdef ROLLOVER_CLOCK
          /* We can still use VERSION_MAX for protecting read-only trasanctions from dirty reads */
          t = VERSION_MAX;
# else /* ! ROLLOVER_CLOCK */
          fprintf(stderr, "Exceeded maximum version number: 0x%lx\n", (unsigned long)t);
          exit(1);
# endif /* ! ROLLOVER_CLOCK */
        }
      }
      ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));
    } else {
      /* Use new incarnation number */
      ATOMIC_STORE_REL(w->lock, LOCK_UPD_INCARNATION(w->version, i));
    }
  }
#elif DESIGN == WRITE_BACK_ETL
  /* Read the lock */
  l = ATOMIC_LOAD_ACQ(lock);
  w = (w_entry_t *)LOCK_GET_ADDR(l);
  /* Reset the write set entry */
  if (tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries) {
    if (w->addr == addr) {
      if (w->next == NULL) {
        /* Release lock */
#if CM == CM_MODULAR
        ATOMIC_CAS_FULL(w->lock, l, LOCK_SET_TIMESTAMP(w->version));
#else
        ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(w->version));
#endif
      } else {
        /* Update lock */
#if CM == CM_MODULAR
        ATOMIC_CAS_FULL(w->lock, l, LOCK_SET_ADDR_WRITE((stm_word_t)w->next));
#else
        ATOMIC_STORE(w->lock, LOCK_SET_ADDR_WRITE((stm_word_t)w->next));
#endif
      }
      w->addr = NULL;
    } else {
      /* Lock was acquired first by another address */
      w->addr = NULL;
    }
  }
#else /* DESIGN == WRITE_BACK_CTL */
  /* Reset the write set entry */
  w = stm_has_written(tx, addr);
  if (w != NULL) {
    w->addr = NULL;
  }
#endif /* DESIGN == WRITE_BACK_CTL */

}
#endif

/*
 * Called by the CURRENT thread to load a word-sized value in a unit transaction.
 */
stm_word_t stm_unit_load(volatile stm_word_t *addr, stm_word_t *timestamp)
{
  volatile stm_word_t *lock;
  stm_word_t l, l2, value;

  PRINT_DEBUG2("==> stm_unit_load(a=%p)\n", addr);

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Read lock, value, lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (LOCK_GET_OWNED(l)) {
    /* Locked: wait until lock is free */
#ifdef WAIT_YIELD
    sched_yield();
#endif /* WAIT_YIELD */
    goto restart;
  }
  /* Not locked */
  value = ATOMIC_LOAD_ACQ(addr);
  l2 = ATOMIC_LOAD_ACQ(lock);
  if (l != l2) {
    l = l2;
    goto restart_no_load;
  }

  if (timestamp != NULL)
    *timestamp = LOCK_GET_TIMESTAMP(l);

  return value;
}

/*
 * Called by the CURRENT thread to store a word-sized value in a unit transaction.
 */
int stm_unit_store(volatile stm_word_t *addr, stm_word_t value, stm_word_t *timestamp)
{
  return stm_unit_write(addr, value, ~(stm_word_t)0, timestamp);
}

/*
 * Called by the CURRENT thread to store part of a word-sized value in a unit transaction.
 */
int stm_unit_store2(volatile stm_word_t *addr, stm_word_t value, stm_word_t mask, stm_word_t *timestamp)
{
  return stm_unit_write(addr, value, mask, timestamp);
}

/*
 * Enable or disable extensions and set upper bound on snapshot.
 */
void stm_set_extension(TXPARAMS int enable, stm_word_t *timestamp)
{
  TX_GET;

  tx->can_extend = enable;
  if (timestamp != NULL && *timestamp < tx->end)
    tx->end = *timestamp;
}

/*
 * Get curent value of global clock.
 */
stm_word_t stm_get_clock()
{
  return GET_CLOCK;
}

/*
 * Get current transaction descriptor.
 */
stm_tx_t *stm_current_tx()
{
  return stm_get_tx();
}

/* ################################################################### *
 * UNDOCUMENTED STM FUNCTIONS (USE WITH CARE!)
 * ################################################################### */

#ifdef CONFLICT_TRACKING
/*
 * Get thread identifier of other transaction.
 */
int stm_get_thread_id(stm_tx_t *tx, pthread_t *id)
{
  *id = tx->thread_id;
  return 1;
}

/*
 * Set global conflict callback.
 */
int stm_set_conflict_cb(void (*on_conflict)(stm_tx_t *tx1, stm_tx_t *tx2))
{
  conflict_cb = on_conflict;
  return 1;
}
#endif /* CONFLICT_TRACKING */

#ifdef IRREVOCABLE_ENABLED
int stm_set_irrevocable(TXPARAMS int serial)
{
# if CM == CM_MODULAR
  stm_word_t t;
# endif /* CM == CM_MODULAR */
  TX_GET;

  if (!IS_ACTIVE(tx->status) && serial != -1) {
    /* Request irrevocability outside of a transaction or in abort handler (for next execution) */
    tx->irrevocable = 1 + (serial ? 0x08 : 0);
    return 0;
  }

  /* Are we already in irrevocable mode? */
  if ((tx->irrevocable & 0x07) == 3) {
    return 1;
  }

  if (tx->irrevocable == 0) {
    /* Acquire irrevocability for the first time */
    tx->irrevocable = 1 + (serial ? 0x08 : 0);
#ifdef HYBRID_ASF
    /* TODO: we shouldn't use pthread_mutex/cond since it could use syscall. */
    if (tx->software == 0) {
      asf_abort(ASF_RETRY_IRREVOCABLE);
      return 0;
    }
#endif /* HYBRID_ASF */
    /* Try acquiring global lock */
    if (irrevocable == 1 || ATOMIC_CAS_FULL(&irrevocable, 0, 1) == 0) {
      /* Transaction will acquire irrevocability after rollback */
      stm_rollback(tx, STM_ABORT_IRREVOCABLE);
      return 0;
    }
    /* Success: remember we have the lock */
    tx->irrevocable++;
    /* Try validating transaction */
    if (!stm_validate(tx)) {
      stm_rollback(tx, STM_ABORT_VALIDATE);
      return 0;
    }

    if (serial && tx->w_set.nb_entries != 0) {
      /* TODO: or commit the transaction when we have the irrevocability. */
      /* Don't mix transactional and direct accesses => restart with direct accesses */
      stm_rollback(tx, STM_ABORT_IRREVOCABLE);
      return 0;
    }
  } else if ((tx->irrevocable & 0x07) == 1) {
    /* Acquire irrevocability after restart (no need to validate) */
    while (irrevocable == 1 || ATOMIC_CAS_FULL(&irrevocable, 0, 1) == 0)
      ;
    /* Success: remember we have the lock */
    tx->irrevocable++;
  }
  assert((tx->irrevocable & 0x07) == 2);

  /* Are we in serial irrevocable mode? */
  if ((tx->irrevocable & 0x08) != 0) {
    /* Stop all other threads */
    if (stm_quiesce(tx, 1) != 0) {
      /* Another thread is quiescing and we are active (trying to acquire irrevocability) */
      assert(serial != -1);
      stm_rollback(tx, STM_ABORT_IRREVOCABLE);
      return 0;
    }
  }

  /* We are in irrevocable mode */
  tx->irrevocable++;

  return 1;
}
#else /* ! IRREVOCABLE_ENABLED */
int stm_set_irrevocable(TXPARAMS int serial)
{
  fprintf(stderr, "Irrevocability is not supported in this configuration\n");
  exit(-1);
  return 1;
}
#endif /* ! IRREVOCABLE_ENABLED */


#ifdef HYBRID_ASF

/* Checking that the write-set can contains at least 256 */
/* XXX ASF can write many times at the same address, thus we can overflow */
#if RW_SET_SIZE <= 256
# error ASF (LLB_256) needs at least 256 entries for the write set.
#endif /* RW_SET_SIZE */

static inline uint64_t rdtsc()
{
    uint64_t hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

stm_word_t hytm_load(TXPARAMS volatile stm_word_t *addr)
{
  volatile stm_word_t *lock;
  stm_word_t l;
  lock = GET_LOCK(addr);
  /* Load descriptor using ASF */
  l = asf_lock_load64((long unsigned int *)lock);
  if (unlikely(LOCK_GET_WRITE(l))) {
    /* a software transaction is currently using this descriptor */
    asf_abort(ASF_RETRY);
    /* unreachable */
    return 0;
  }
  /* addr can return inconsistent value but will be abort after few cycles */
  return *addr;
  /* Load value using ASF.
   * (in previous version of PTLSIM/ASF, the ordering was not respected).
   * TODO: to be removed. */
  /*return asf_lock_load64((long unsigned int *)addr);*/
}

void hytm_store(TXPARAMS volatile stm_word_t *addr, stm_word_t value)
{
  TX_GET;
  volatile stm_word_t *lock;
  stm_word_t l;
  lock = GET_LOCK(addr);
  /* Load descriptor using ASF */
  l = asf_lock_load64((long unsigned int *)lock);
  if (unlikely(LOCK_GET_WRITE(l))) {
    /* a software transaction is currently using this descriptor, ASF tx has to give up */
    asf_abort(ASF_RETRY);
    /* XXX mark as unreachable */
    return;
  }
  /* Write the value using ASF */
  asf_lock_store64((long unsigned int *)addr, value);
  /* Add to write set to update the locks when we acquire TS */
  /* XXX This could overflow if many write to the same address. */
  tx->w_set.entries[tx->w_set.nb_entries++].lock = lock;
}

void hytm_store2(TXPARAMS volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  hytm_store(TXARGS addr, (asf_lock_load64((long unsigned int *)addr) & ~mask) | (value & mask));
}

int hytm_commit(TXPARAM) 
{
  stm_word_t t;
  w_entry_t *w;
  int i;
  TX_GET;

  /* Release irrevocability */
#ifdef IRREVOCABLE_ENABLED
  if (tx->irrevocable) {
    ATOMIC_STORE(&irrevocable, 0);
    if ((tx->irrevocable & 0x08) != 0)
      stm_quiesce_release(tx);
    tx->irrevocable = 0;
    goto commit_end;
  }
#endif /* IRREVOCABLE_ENABLED */

  t = FETCH_INC_CLOCK + 1;
  
  /* Set new timestamp in locks */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    /* XXX Maybe no duplicate entries can improve perf? */
    asf_lock_store64((long unsigned int *)w->lock, LOCK_SET_TIMESTAMP(t));
  }
  /* Commit the hytm transaction */
  asf_commit();

commit_end:
  tx->retries = 0;
  /* Set status to COMMITTED */
  SET_STATUS(tx->status, TX_COMMITTED);

  /* TODO statistics */
#ifdef INTERNAL_STATS
  
#endif
  return 1;
}

void hytm_abort(TXPARAMS int reason)
{
  asf_abort(ASF_RETRY);
}

sigjmp_buf *hytm_start(TXPARAMS stm_tx_attr_t *attr)
{
  TX_GET;
  unsigned long err;

  tx->retries = 0;
  /* Set status */
  UPDATE_STATUS(tx->status, TX_ACTIVE);
  stm_check_quiesce(tx);
  /* copy attributes if need to switch to software mode */
  tx->attr = (attr == NULL ? default_attributes : *attr);
  tx->start = rdtsc();

hytm_restart:
  /* All registers are lost when ASF aborts, thus we discard registers */
  asm volatile (ASF_SPECULATE 
                :"=a" (err)
                :
                :"memory","rbp","rbx","rcx","rdx","rsi","rdi",
                 "r8", "r9","r10","r11","r12","r13","r14","r15" );
 
  tx = stm_get_tx();
  if (unlikely(asf_status_code(err) != 0)) {
    /* Set status to ABORTED */
    SET_STATUS(tx->status, TX_ABORTED);
    tx->retries++;
    /* Error management */
    if (asf_status_code(err) == ASF_STATUS_CONTENTION) {
      if (tx->retries > ASF_ABORT_THRESHOLD) {
        /* There is too many conflicts, software will not help, start irrevocability. */
        stm_set_irrevocable(TXARGS 1);  /* Set irrevocable serial */
#if defined(TM_DTMC)
        stm_set_irrevocable(TXARGS -1); /* Acquire irrevocability */
        UPDATE_STATUS(tx->status, TX_IRREVOCABLE);
        siglongjmp(tx->env, 0x02); /* ABI 0x02 = runUninstrumented */
#else /* ! defined(TM_DTMC) */
        /* Non-tm compiler and GCC doesn't have path without instrumentation. */
        tx->software = 1;
#endif /* ! defined(TM_DTMC) */
      }
    } else if (asf_status_code(err) == ASF_STATUS_ABORT) {
      if (asf_abort_code(err) == ASF_FORCE_SOFTWARE) {
        tx->software = 1;
#ifdef IRREVOCABLE_ENABLED
      } else if(asf_abort_code(err) == ASF_RETRY_IRREVOCABLE) {
#if defined(TM_DTMC)
        if (tx->irrevocable != 0) {
          stm_set_irrevocable(TXARGS -1);
          UPDATE_STATUS(tx->status, TX_IRREVOCABLE);
          siglongjmp(tx->env, 0x02); /* ABI 0x02 = runUninstrumented */
        } 
#else /* ! defined(TM_DTMC) */
        /* Non-tm compiler and GCC doesn't have path without instrumentation. */
        tx->software = 1;
#endif /* ! defined(TM_DTMC) */
#endif /* IRREVOCABLE_ENABLED */
      } else {
        if (tx->retries > ASF_ABORT_THRESHOLD) { 
          tx->software = 1;
        }
      }
    } else {
      /* Other cases are critical and needs software mode */
      tx->software = 1;
    }
    if (tx->software) {
      /* Start a software transaction (it cannot use attr since the register/stack can be corrupted) */
      stm_start(TXARGS &tx->attr);
      /* Restoring the context */
#if defined(TM_DTMC)
      siglongjmp(tx->env, 0x01); /* ABI 0x01 = runInstrumented, DTMC explicitly needs 1 */
#else /* ! defined(TM_DTMC) */
      siglongjmp(tx->env, 0x09); /* ABI 0x09 = runInstrumented + restoreLiveVariable */
#endif /* ! defined(TM_DTMC) */
    } else {
      uint64_t wait = (uint64_t)rdtsc + (random() % (rdtsc() - tx->start)); /* XXX random but maybe not reliable */
      /* Waiting... */
      while (rdtsc() < wait);
      UPDATE_STATUS(tx->status, TX_ACTIVE);
      /* Check quiesce before to restart */
      stm_check_quiesce(tx);
      goto hytm_restart;
    }
  }
 
  /* Reset write set */
  tx->w_set.nb_entries = 0;

  if (tx->retries > 0) {
    /* Restoring registers for retry */
#if defined(TM_DTMC)
    siglongjmp(tx->env, 0x01); /* ABI 0x01 = runInstrumented, DTMC explicitly needs 1 */
#else /* ! defined(TM_DTMC) */
    siglongjmp(tx->env, 0x09); /* ABI 0x09 = runInstrumented + restoreLiveVariable */
#endif /* ! defined(TM_DTMC) */
  } 

  return &tx->env;
}

/* generic interface for HYTM and STM */

sigjmp_buf *tm_start(TXPARAMS stm_tx_attr_t *attr)
{
  TX_GET;

  if (!tx->software) {
    return hytm_start(TXARGS attr);
  } else {
    return stm_start(TXARGS attr);
  }
}

stm_word_t tm_load(TXPARAMS volatile stm_word_t *addr)
{
  TX_GET;
  if (!tx->software) {
    return hytm_load(TXARGS addr);
  } else {
    return stm_load(TXARGS addr);
  }
}

void tm_store(TXPARAMS volatile stm_word_t *addr, stm_word_t value)
{
  TX_GET;
  if (!tx->software) {
    hytm_store(TXARGS addr, value);
  } else {
    stm_store(TXARGS addr, value);
  }
}

void tm_store2(TXPARAMS volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  TX_GET;
  if (!tx->software) {
    hytm_store2(TXARGS addr, value, mask);
  } else {
    stm_store2(TXARGS addr, value, mask);
  }
}

int tm_commit(TXPARAM)
{
  TX_GET;
  if (!tx->software) {
    return hytm_commit(TXARG);
  } else {
    return stm_commit(TXARG);
  }
}

void tm_abort(TXPARAMS int reason)
{
  TX_GET;
  if (!tx->software) {
    hytm_abort(TXARGS reason);
  } else {
    stm_abort(TXARGS reason);
  }
}

int tm_hybrid(TXPARAM)
{
  TX_GET;
  return !tx->software;
}

void tm_restart_software(TXPARAM)
{
  TX_GET;
  if (!tx->software) {
    asf_abort(ASF_FORCE_SOFTWARE);
  }
}
#else
/* Define tm_* functions */

sigjmp_buf *tm_start(TXPARAMS stm_tx_attr_t *attr)
{
  return stm_start(TXARGS attr);
}
# if CM == CM_MODULAR
   /* We might still abort if we cannot set status (e.g., we are being killed) */
    t = tx->status;
    if (GET_STATUS(t) != TX_ACTIVE || ATOMIC_CAS_FULL(&tx->status, t, t + (TX_IRREVOCABLE - TX_ACTIVE)) == 0) {
      stm_rollback(tx, STM_ABORT_KILLED);
      return 0;
    }
# endif /* CM == CM_MODULAR */
stm_word_t tm_load(TXPARAMS volatile stm_word_t *addr)
{
  return stm_load(TXARGS addr);
}

void tm_store(TXPARAMS volatile stm_word_t *addr, stm_word_t value)
{
  stm_store(TXARGS addr, value);
}

void tm_store2(TXPARAMS volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  stm_store2(TXARGS addr, value, mask);
}

int tm_commit(TXPARAM)
{
  return stm_commit(TXARG);
}
void tm_abort(TXPARAMS int reason)
{
  stm_abort(TXARGS reason);
}

/* TODO what should be done for hybrid specific functions */
void tm_restart_software(TXPARAM) {
}
int tm_hybrid(TXPARAM) {
  return 0;
}


#endif /* HYBRID_ASF */

