/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Written by Mark Hemment, 1996 (markhe@nextd.demon.co.uk).
 *
 * (C) SGI 2006, Christoph Lameter
 * 	Cleaned up and restructured to ease the addition of alternative
 * 	implementations of SLAB allocators.
 * (C) Linux Foundation 2008-2013
 *      Unified interface for all slab allocators
 */

#ifndef _LINUX_SLAB_H
#define	_LINUX_SLAB_H

#include <linux/cache.h>
#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/percpu-refcount.h>
#include <linux/cleanup.h>
#include <linux/hash.h>

enum _slab_flag_bits {
	_SLAB_CONSISTENCY_CHECKS,
	_SLAB_RED_ZONE,
	_SLAB_POISON,
	_SLAB_KMALLOC,
	_SLAB_HWCACHE_ALIGN,
	_SLAB_CACHE_DMA,
	_SLAB_CACHE_DMA32,
	_SLAB_STORE_USER,
	_SLAB_PANIC,
	_SLAB_TYPESAFE_BY_RCU,
	_SLAB_TRACE,
#ifdef CONFIG_DEBUG_OBJECTS
	_SLAB_DEBUG_OBJECTS,
#endif
	_SLAB_NOLEAKTRACE,
	_SLAB_NO_MERGE,
#ifdef CONFIG_FAILSLAB
	_SLAB_FAILSLAB,
#endif
#ifdef CONFIG_MEMCG
	_SLAB_ACCOUNT,
#endif
#ifdef CONFIG_KASAN_GENERIC
	_SLAB_KASAN,
#endif
	_SLAB_NO_USER_FLAGS,
#ifdef CONFIG_KFENCE
	_SLAB_SKIP_KFENCE,
#endif
#ifndef CONFIG_SLUB_TINY
	_SLAB_RECLAIM_ACCOUNT,
#endif
	_SLAB_OBJECT_POISON,
	_SLAB_CMPXCHG_DOUBLE,
#ifdef CONFIG_SLAB_OBJ_EXT
	_SLAB_NO_OBJ_EXT,
#endif
	_SLAB_FLAGS_LAST_BIT
};

#define __SLAB_FLAG_BIT(nr)	((slab_flags_t __force)(1U << (nr)))
#define __SLAB_FLAG_UNUSED	((slab_flags_t __force)(0U))

/*
 * Flags to pass to kmem_cache_create().
 * The ones marked DEBUG need CONFIG_SLUB_DEBUG enabled, otherwise are no-op
 */
/* DEBUG: Perform (expensive) checks on alloc/free */
#define SLAB_CONSISTENCY_CHECKS	__SLAB_FLAG_BIT(_SLAB_CONSISTENCY_CHECKS)
/* DEBUG: Red zone objs in a cache */
#define SLAB_RED_ZONE		__SLAB_FLAG_BIT(_SLAB_RED_ZONE)
/* DEBUG: Poison objects */
#define SLAB_POISON		__SLAB_FLAG_BIT(_SLAB_POISON)
/* Indicate a kmalloc slab */
#define SLAB_KMALLOC		__SLAB_FLAG_BIT(_SLAB_KMALLOC)
/**
 * define SLAB_HWCACHE_ALIGN - Align objects on cache line boundaries.
 *
 * Sufficiently large objects are aligned on cache line boundary. For object
 * size smaller than a half of cache line size, the alignment is on the half of
 * cache line size. In general, if object size is smaller than 1/2^n of cache
 * line size, the alignment is adjusted to 1/2^n.
 *
 * If explicit alignment is also requested by the respective
 * &struct kmem_cache_args field, the greater of both is alignments is applied.
 */
#define SLAB_HWCACHE_ALIGN	__SLAB_FLAG_BIT(_SLAB_HWCACHE_ALIGN)
/* Use GFP_DMA memory */
#define SLAB_CACHE_DMA		__SLAB_FLAG_BIT(_SLAB_CACHE_DMA)
/* Use GFP_DMA32 memory */
#define SLAB_CACHE_DMA32	__SLAB_FLAG_BIT(_SLAB_CACHE_DMA32)
/* DEBUG: Store the last owner for bug hunting */
#define SLAB_STORE_USER		__SLAB_FLAG_BIT(_SLAB_STORE_USER)
/* Panic if kmem_cache_create() fails */
#define SLAB_PANIC		__SLAB_FLAG_BIT(_SLAB_PANIC)
/**
 * define SLAB_TYPESAFE_BY_RCU - **WARNING** READ THIS!
 *
 * This delays freeing the SLAB page by a grace period, it does _NOT_
 * delay object freeing. This means that if you do kmem_cache_free()
 * that memory location is free to be reused at any time. Thus it may
 * be possible to see another object there in the same RCU grace period.
 *
 * This feature only ensures the memory location backing the object
 * stays valid, the trick to using this is relying on an independent
 * object validation pass. Something like:
 *
 * ::
 *
 *  begin:
 *   rcu_read_lock();
 *   obj = lockless_lookup(key);
 *   if (obj) {
 *     if (!try_get_ref(obj)) // might fail for free objects
 *       rcu_read_unlock();
 *       goto begin;
 *
 *     if (obj->key != key) { // not the object we expected
 *       put_ref(obj);
 *       rcu_read_unlock();
 *       goto begin;
 *     }
 *   }
 *  rcu_read_unlock();
 *
 * This is useful if we need to approach a kernel structure obliquely,
 * from its address obtained without the usual locking. We can lock
 * the structure to stabilize it and check it's still at the given address,
 * only if we can be sure that the memory has not been meanwhile reused
 * for some other kind of object (which our subsystem's lock might corrupt).
 *
 * rcu_read_lock before reading the address, then rcu_read_unlock after
 * taking the spinlock within the structure expected at that address.
 *
 * Note that object identity check has to be done *after* acquiring a
 * reference, therefore user has to ensure proper ordering for loads.
 * Similarly, when initializing objects allocated with SLAB_TYPESAFE_BY_RCU,
 * the newly allocated object has to be fully initialized *before* its
 * refcount gets initialized and proper ordering for stores is required.
 * refcount_{add|inc}_not_zero_acquire() and refcount_set_release() are
 * designed with the proper fences required for reference counting objects
 * allocated with SLAB_TYPESAFE_BY_RCU.
 *
 * Note that it is not possible to acquire a lock within a structure
 * allocated with SLAB_TYPESAFE_BY_RCU without first acquiring a reference
 * as described above.  The reason is that SLAB_TYPESAFE_BY_RCU pages
 * are not zeroed before being given to the slab, which means that any
 * locks must be initialized after each and every kmem_struct_alloc().
 * Alternatively, make the ctor passed to kmem_cache_create() initialize
 * the locks at page-allocation time, as is done in __i915_request_ctor(),
 * sighand_ctor(), and anon_vma_ctor().  Such a ctor permits readers
 * to safely acquire those ctor-initialized locks under rcu_read_lock()
 * protection.
 *
 * Note that SLAB_TYPESAFE_BY_RCU was originally named SLAB_DESTROY_BY_RCU.
 */
#define SLAB_TYPESAFE_BY_RCU	__SLAB_FLAG_BIT(_SLAB_TYPESAFE_BY_RCU)
/* Trace allocations and frees */
#define SLAB_TRACE		__SLAB_FLAG_BIT(_SLAB_TRACE)

/* Flag to prevent checks on free */
#ifdef CONFIG_DEBUG_OBJECTS
# define SLAB_DEBUG_OBJECTS	__SLAB_FLAG_BIT(_SLAB_DEBUG_OBJECTS)
#else
# define SLAB_DEBUG_OBJECTS	__SLAB_FLAG_UNUSED
#endif

/* Avoid kmemleak tracing */
#define SLAB_NOLEAKTRACE	__SLAB_FLAG_BIT(_SLAB_NOLEAKTRACE)

/*
 * Prevent merging with compatible kmem caches. This flag should be used
 * cautiously. Valid use cases:
 *
 * - caches created for self-tests (e.g. kunit)
 * - general caches created and used by a subsystem, only when a
 *   (subsystem-specific) debug option is enabled
 * - performance critical caches, should be very rare and consulted with slab
 *   maintainers, and not used together with CONFIG_SLUB_TINY
 */
#define SLAB_NO_MERGE		__SLAB_FLAG_BIT(_SLAB_NO_MERGE)

/* Fault injection mark */
#ifdef CONFIG_FAILSLAB
# define SLAB_FAILSLAB		__SLAB_FLAG_BIT(_SLAB_FAILSLAB)
#else
# define SLAB_FAILSLAB		__SLAB_FLAG_UNUSED
#endif
/**
 * define SLAB_ACCOUNT - Account allocations to memcg.
 *
 * All object allocations from this cache will be memcg accounted, regardless of
 * __GFP_ACCOUNT being or not being passed to individual allocations.
 */
#ifdef CONFIG_MEMCG
# define SLAB_ACCOUNT		__SLAB_FLAG_BIT(_SLAB_ACCOUNT)
#else
# define SLAB_ACCOUNT		__SLAB_FLAG_UNUSED
#endif

#ifdef CONFIG_KASAN_GENERIC
#define SLAB_KASAN		__SLAB_FLAG_BIT(_SLAB_KASAN)
#else
#define SLAB_KASAN		__SLAB_FLAG_UNUSED
#endif

/*
 * Ignore user specified debugging flags.
 * Intended for caches created for self-tests so they have only flags
 * specified in the code and other flags are ignored.
 */
#define SLAB_NO_USER_FLAGS	__SLAB_FLAG_BIT(_SLAB_NO_USER_FLAGS)

#ifdef CONFIG_KFENCE
#define SLAB_SKIP_KFENCE	__SLAB_FLAG_BIT(_SLAB_SKIP_KFENCE)
#else
#define SLAB_SKIP_KFENCE	__SLAB_FLAG_UNUSED
#endif

/* The following flags affect the page allocator grouping pages by mobility */
/**
 * define SLAB_RECLAIM_ACCOUNT - Objects are reclaimable.
 *
 * Use this flag for caches that have an associated shrinker. As a result, slab
 * pages are allocated with __GFP_RECLAIMABLE, which affects grouping pages by
 * mobility, and are accounted in SReclaimable counter in /proc/meminfo
 */
#ifndef CONFIG_SLUB_TINY
#define SLAB_RECLAIM_ACCOUNT	__SLAB_FLAG_BIT(_SLAB_RECLAIM_ACCOUNT)
#else
#define SLAB_RECLAIM_ACCOUNT	__SLAB_FLAG_UNUSED
#endif
#define SLAB_TEMPORARY		SLAB_RECLAIM_ACCOUNT	/* Objects are short-lived */

/* Slab created using create_boot_cache */
#ifdef CONFIG_SLAB_OBJ_EXT
#define SLAB_NO_OBJ_EXT		__SLAB_FLAG_BIT(_SLAB_NO_OBJ_EXT)
#else
#define SLAB_NO_OBJ_EXT		__SLAB_FLAG_UNUSED
#endif

/*
 * ZERO_SIZE_PTR will be returned for zero sized kmalloc requests.
 *
 * Dereferencing ZERO_SIZE_PTR will lead to a distinct access fault.
 *
 * ZERO_SIZE_PTR can be passed to kfree though in the same way that NULL can.
 * Both make kfree a no-op.
 */
#define ZERO_SIZE_PTR ((void *)16)

#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= \
				(unsigned long)ZERO_SIZE_PTR)

#include <linux/kasan.h>

struct list_lru;
struct mem_cgroup;
/*
 * struct kmem_cache related prototypes
 */
bool slab_is_available(void);

/**
 * struct kmem_cache_args - Less common arguments for kmem_cache_create()
 *
 * Any uninitialized fields of the structure are interpreted as unused. The
 * exception is @freeptr_offset where %0 is a valid value, so
 * @use_freeptr_offset must be also set to %true in order to interpret the field
 * as used. For @useroffset %0 is also valid, but only with non-%0
 * @usersize.
 *
 * When %NULL args is passed to kmem_cache_create(), it is equivalent to all
 * fields unused.
 */
struct kmem_cache_args {
	/**
	 * @align: The required alignment for the objects.
	 *
	 * %0 means no specific alignment is requested.
	 */
	unsigned int align;
	/**
	 * @useroffset: Usercopy region offset.
	 *
	 * %0 is a valid offset, when @usersize is non-%0
	 */
	unsigned int useroffset;
	/**
	 * @usersize: Usercopy region size.
	 *
	 * %0 means no usercopy region is specified.
	 */
	unsigned int usersize;
	/**
	 * @freeptr_offset: Custom offset for the free pointer
	 * in &SLAB_TYPESAFE_BY_RCU caches
	 *
	 * By default &SLAB_TYPESAFE_BY_RCU caches place the free pointer
	 * outside of the object. This might cause the object to grow in size.
	 * Cache creators that have a reason to avoid this can specify a custom
	 * free pointer offset in their struct where the free pointer will be
	 * placed.
	 *
	 * Note that placing the free pointer inside the object requires the
	 * caller to ensure that no fields are invalidated that are required to
	 * guard against object recycling (See &SLAB_TYPESAFE_BY_RCU for
	 * details).
	 *
	 * Using %0 as a value for @freeptr_offset is valid. If @freeptr_offset
	 * is specified, %use_freeptr_offset must be set %true.
	 *
	 * Note that @ctor currently isn't supported with custom free pointers
	 * as a @ctor requires an external free pointer.
	 */
	unsigned int freeptr_offset;
	/**
	 * @use_freeptr_offset: Whether a @freeptr_offset is used.
	 */
	bool use_freeptr_offset;
	/**
	 * @ctor: A constructor for the objects.
	 *
	 * The constructor is invoked for each object in a newly allocated slab
	 * page. It is the cache user's responsibility to free object in the
	 * same state as after calling the constructor, or deal appropriately
	 * with any differences between a freshly constructed and a reallocated
	 * object.
	 *
	 * %NULL means no constructor.
	 */
	void (*ctor)(void *);
};

struct kmem_cache *__kmem_cache_create_args(const char *name,
					    unsigned int object_size,
					    struct kmem_cache_args *args,
					    slab_flags_t flags);
static inline struct kmem_cache *
__kmem_cache_create(const char *name, unsigned int size, unsigned int align,
		    slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache_args kmem_args = {
		.align	= align,
		.ctor	= ctor,
	};

	return __kmem_cache_create_args(name, size, &kmem_args, flags);
}

/**
 * kmem_cache_create_usercopy - Create a kmem cache with a region suitable
 * for copying to userspace.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: SLAB flags
 * @useroffset: Usercopy region offset
 * @usersize: Usercopy region size
 * @ctor: A constructor for the objects, or %NULL.
 *
 * This is a legacy wrapper, new code should use either KMEM_CACHE_USERCOPY()
 * if whitelisting a single field is sufficient, or kmem_cache_create() with
 * the necessary parameters passed via the args parameter (see
 * &struct kmem_cache_args)
 *
 * Return: a pointer to the cache on success, NULL on failure.
 */
static inline struct kmem_cache *
kmem_cache_create_usercopy(const char *name, unsigned int size,
			   unsigned int align, slab_flags_t flags,
			   unsigned int useroffset, unsigned int usersize,
			   void (*ctor)(void *))
{
	struct kmem_cache_args kmem_args = {
		.align		= align,
		.ctor		= ctor,
		.useroffset	= useroffset,
		.usersize	= usersize,
	};

	return __kmem_cache_create_args(name, size, &kmem_args, flags);
}

/* If NULL is passed for @args, use this variant with default arguments. */
static inline struct kmem_cache *
__kmem_cache_default_args(const char *name, unsigned int size,
			  struct kmem_cache_args *args,
			  slab_flags_t flags)
{
	struct kmem_cache_args kmem_default_args = {};

	/* Make sure we don't get passed garbage. */
	if (WARN_ON_ONCE(args))
		return ERR_PTR(-EINVAL);

	return __kmem_cache_create_args(name, size, &kmem_default_args, flags);
}

/**
 * kmem_cache_create - Create a kmem cache.
 * @__name: A string which is used in /proc/slabinfo to identify this cache.
 * @__object_size: The size of objects to be created in this cache.
 * @__args: Optional arguments, see &struct kmem_cache_args. Passing %NULL
 *	    means defaults will be used for all the arguments.
 *
 * This is currently implemented as a macro using ``_Generic()`` to call
 * either the new variant of the function, or a legacy one.
 *
 * The new variant has 4 parameters:
 * ``kmem_cache_create(name, object_size, args, flags)``
 *
 * See __kmem_cache_create_args() which implements this.
 *
 * The legacy variant has 5 parameters:
 * ``kmem_cache_create(name, object_size, align, flags, ctor)``
 *
 * The align and ctor parameters map to the respective fields of
 * &struct kmem_cache_args
 *
 * Context: Cannot be called within a interrupt, but can be interrupted.
 *
 * Return: a pointer to the cache on success, NULL on failure.
 */
#define kmem_cache_create(__name, __object_size, __args, ...)           \
	_Generic((__args),                                              \
		struct kmem_cache_args *: __kmem_cache_create_args,	\
		void *: __kmem_cache_default_args,			\
		default: __kmem_cache_create)(__name, __object_size, __args, __VA_ARGS__)

void kmem_cache_destroy(struct kmem_cache *s);
int kmem_cache_shrink(struct kmem_cache *s);

/*
 * Please use this macro to create slab caches. Simply specify the
 * name of the structure and maybe some flags that are listed above.
 *
 * The alignment of the struct determines object alignment. If you
 * f.e. add ____cacheline_aligned_in_smp to the struct declaration
 * then the objects will be properly aligned in SMP configurations.
 */
#define KMEM_CACHE(__struct, __flags)                                   \
	__kmem_cache_create_args(#__struct, sizeof(struct __struct),    \
			&(struct kmem_cache_args) {			\
				.align	= __alignof__(struct __struct), \
			}, (__flags))

/*
 * To whitelist a single field for copying to/from usercopy, use this
 * macro instead for KMEM_CACHE() above.
 */
#define KMEM_CACHE_USERCOPY(__struct, __flags, __field)						\
	__kmem_cache_create_args(#__struct, sizeof(struct __struct),				\
			&(struct kmem_cache_args) {						\
				.align		= __alignof__(struct __struct),			\
				.useroffset	= offsetof(struct __struct, __field),		\
				.usersize	= sizeof_field(struct __struct, __field),	\
			}, (__flags))

/*
 * Common kmalloc functions provided by all allocators
 */
void * __must_check krealloc_noprof(const void *objp, size_t new_size,
				    gfp_t flags) __realloc_size(2);
#define krealloc(...)				alloc_hooks(krealloc_noprof(__VA_ARGS__))

void kfree(const void *objp);
void kfree_sensitive(const void *objp);
size_t __ksize(const void *objp);

DEFINE_FREE(kfree, void *, if (!IS_ERR_OR_NULL(_T)) kfree(_T))
DEFINE_FREE(kfree_sensitive, void *, if (_T) kfree_sensitive(_T))

/**
 * ksize - Report actual allocation size of associated object
 *
 * @objp: Pointer returned from a prior kmalloc()-family allocation.
 *
 * This should not be used for writing beyond the originally requested
 * allocation size. Either use krealloc() or round up the allocation size
 * with kmalloc_size_roundup() prior to allocation. If this is used to
 * access beyond the originally requested allocation size, UBSAN_BOUNDS
 * and/or FORTIFY_SOURCE may trip, since they only know about the
 * originally allocated size via the __alloc_size attribute.
 */
size_t ksize(const void *objp);

#ifdef CONFIG_PRINTK
bool kmem_dump_obj(void *object);
#else
static inline bool kmem_dump_obj(void *object) { return false; }
#endif

/*
 * Some archs want to perform DMA into kmalloc caches and need a guaranteed
 * alignment larger than the alignment of a 64-bit integer.
 * Setting ARCH_DMA_MINALIGN in arch headers allows that.
 */
#ifdef ARCH_HAS_DMA_MINALIGN
#if ARCH_DMA_MINALIGN > 8 && !defined(ARCH_KMALLOC_MINALIGN)
#define ARCH_KMALLOC_MINALIGN ARCH_DMA_MINALIGN
#endif
#endif

#ifndef ARCH_KMALLOC_MINALIGN
#define ARCH_KMALLOC_MINALIGN __alignof__(unsigned long long)
#elif ARCH_KMALLOC_MINALIGN > 8
#define KMALLOC_MIN_SIZE ARCH_KMALLOC_MINALIGN
#define KMALLOC_SHIFT_LOW ilog2(KMALLOC_MIN_SIZE)
#endif

/*
 * Setting ARCH_SLAB_MINALIGN in arch headers allows a different alignment.
 * Intended for arches that get misalignment faults even for 64 bit integer
 * aligned buffers.
 */
#ifndef ARCH_SLAB_MINALIGN
#define ARCH_SLAB_MINALIGN __alignof__(unsigned long long)
#endif

/*
 * Arches can define this function if they want to decide the minimum slab
 * alignment at runtime. The value returned by the function must be a power
 * of two and >= ARCH_SLAB_MINALIGN.
 */
#ifndef arch_slab_minalign
static inline unsigned int arch_slab_minalign(void)
{
	return ARCH_SLAB_MINALIGN;
}
#endif

/*
 * kmem_cache_alloc and friends return pointers aligned to ARCH_SLAB_MINALIGN.
 * kmalloc and friends return pointers aligned to both ARCH_KMALLOC_MINALIGN
 * and ARCH_SLAB_MINALIGN, but here we only assume the former alignment.
 */
#define __assume_kmalloc_alignment __assume_aligned(ARCH_KMALLOC_MINALIGN)
#define __assume_slab_alignment __assume_aligned(ARCH_SLAB_MINALIGN)
#define __assume_page_alignment __assume_aligned(PAGE_SIZE)

/*
 * Kmalloc array related definitions
 */

/*
 * SLUB directly allocates requests fitting in to an order-1 page
 * (PAGE_SIZE*2).  Larger requests are passed to the page allocator.
 */
#define KMALLOC_SHIFT_HIGH	(PAGE_SHIFT + 1)
#define KMALLOC_SHIFT_MAX	(MAX_PAGE_ORDER + PAGE_SHIFT)
#ifndef KMALLOC_SHIFT_LOW
#define KMALLOC_SHIFT_LOW	3
#endif

/* Maximum allocatable size */
#define KMALLOC_MAX_SIZE	(1UL << KMALLOC_SHIFT_MAX)
/* Maximum size for which we actually use a slab cache */
#define KMALLOC_MAX_CACHE_SIZE	(1UL << KMALLOC_SHIFT_HIGH)
/* Maximum order allocatable via the slab allocator */
#define KMALLOC_MAX_ORDER	(KMALLOC_SHIFT_MAX - PAGE_SHIFT)

/*
 * Kmalloc subsystem.
 */
#ifndef KMALLOC_MIN_SIZE
#define KMALLOC_MIN_SIZE (1 << KMALLOC_SHIFT_LOW)
#endif

/*
 * This restriction comes from byte sized index implementation.
 * Page size is normally 2^12 bytes and, in this case, if we want to use
 * byte sized index which can represent 2^8 entries, the size of the object
 * should be equal or greater to 2^12 / 2^8 = 2^4 = 16.
 * If minimum size of kmalloc is less than 16, we use it as minimum object
 * size and give up to use byte sized index.
 */
#define SLAB_OBJ_MIN_SIZE      (KMALLOC_MIN_SIZE < 16 ? \
                               (KMALLOC_MIN_SIZE) : 16)

#ifdef CONFIG_RANDOM_KMALLOC_CACHES
#define RANDOM_KMALLOC_CACHES_NR	15 // # of cache copies
#else
#define RANDOM_KMALLOC_CACHES_NR	0
#endif

/*
 * Whenever changing this, take care of that kmalloc_type() and
 * create_kmalloc_caches() still work as intended.
 *
 * KMALLOC_NORMAL can contain only unaccounted objects whereas KMALLOC_CGROUP
 * is for accounted but unreclaimable and non-dma objects. All the other
 * kmem caches can have both accounted and unaccounted objects.
 */
enum kmalloc_cache_type {
	KMALLOC_NORMAL = 0,
#ifndef CONFIG_ZONE_DMA
	KMALLOC_DMA = KMALLOC_NORMAL,
#endif
#ifndef CONFIG_MEMCG
	KMALLOC_CGROUP = KMALLOC_NORMAL,
#endif
	KMALLOC_RANDOM_START = KMALLOC_NORMAL,
	KMALLOC_RANDOM_END = KMALLOC_RANDOM_START + RANDOM_KMALLOC_CACHES_NR,
#ifdef CONFIG_SLUB_TINY
	KMALLOC_RECLAIM = KMALLOC_NORMAL,
#else
	KMALLOC_RECLAIM,
#endif
#ifdef CONFIG_ZONE_DMA
	KMALLOC_DMA,
#endif
#ifdef CONFIG_MEMCG
	KMALLOC_CGROUP,
#endif
	NR_KMALLOC_TYPES
};

typedef struct kmem_cache * kmem_buckets[KMALLOC_SHIFT_HIGH + 1];

extern kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES];

/*
 * Define gfp bits that should not be set for KMALLOC_NORMAL.
 */
#define KMALLOC_NOT_NORMAL_BITS					\
	(__GFP_RECLAIMABLE |					\
	(IS_ENABLED(CONFIG_ZONE_DMA)   ? __GFP_DMA : 0) |	\
	(IS_ENABLED(CONFIG_MEMCG) ? __GFP_ACCOUNT : 0))

extern unsigned long random_kmalloc_seed;

static __always_inline enum kmalloc_cache_type kmalloc_type(gfp_t flags, unsigned long caller)
{
	/*
	 * The most common case is KMALLOC_NORMAL, so test for it
	 * with a single branch for all the relevant flags.
	 */
	if (likely((flags & KMALLOC_NOT_NORMAL_BITS) == 0))
#ifdef CONFIG_RANDOM_KMALLOC_CACHES
		/* RANDOM_KMALLOC_CACHES_NR (=15) copies + the KMALLOC_NORMAL */
		return KMALLOC_RANDOM_START + hash_64(caller ^ random_kmalloc_seed,
						      ilog2(RANDOM_KMALLOC_CACHES_NR + 1));
#else
		return KMALLOC_NORMAL;
#endif

	/*
	 * At least one of the flags has to be set. Their priorities in
	 * decreasing order are:
	 *  1) __GFP_DMA
	 *  2) __GFP_RECLAIMABLE
	 *  3) __GFP_ACCOUNT
	 */
	if (IS_ENABLED(CONFIG_ZONE_DMA) && (flags & __GFP_DMA))
		return KMALLOC_DMA;
	if (!IS_ENABLED(CONFIG_MEMCG) || (flags & __GFP_RECLAIMABLE))
		return KMALLOC_RECLAIM;
	else
		return KMALLOC_CGROUP;
}

/*
 * Figure out which kmalloc slab an allocation of a certain size
 * belongs to.
 * 0 = zero alloc
 * 1 =  65 .. 96 bytes
 * 2 = 129 .. 192 bytes
 * n = 2^(n-1)+1 .. 2^n
 *
 * Note: __kmalloc_index() is compile-time optimized, and not runtime optimized;
 * typical usage is via kmalloc_index() and therefore evaluated at compile-time.
 * Callers where !size_is_constant should only be test modules, where runtime
 * overheads of __kmalloc_index() can be tolerated.  Also see kmalloc_slab().
 */
static __always_inline unsigned int __kmalloc_index(size_t size,
						    bool size_is_constant)
{
	if (!size)
		return 0;

	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;

	if (KMALLOC_MIN_SIZE <= 32 && size > 64 && size <= 96)
		return 1;
	if (KMALLOC_MIN_SIZE <= 64 && size > 128 && size <= 192)
		return 2;
	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
	if (size <=   8 * 1024) return 13;
	if (size <=  16 * 1024) return 14;
	if (size <=  32 * 1024) return 15;
	if (size <=  64 * 1024) return 16;
	if (size <= 128 * 1024) return 17;
	if (size <= 256 * 1024) return 18;
	if (size <= 512 * 1024) return 19;
	if (size <= 1024 * 1024) return 20;
	if (size <=  2 * 1024 * 1024) return 21;

	if (!IS_ENABLED(CONFIG_PROFILE_ALL_BRANCHES) && size_is_constant)
		BUILD_BUG_ON_MSG(1, "unexpected size in kmalloc_index()");
	else
		BUG();

	/* Will never be reached. Needed because the compiler may complain */
	return -1;
}
static_assert(PAGE_SHIFT <= 20);
#define kmalloc_index(s) __kmalloc_index(s, true)

#include <linux/alloc_tag.h>

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.
 * See kmem_cache_zalloc() for a shortcut of adding __GFP_ZERO to flags.
 *
 * Return: pointer to the new object or %NULL in case of error
 */
void *kmem_cache_alloc_noprof(struct kmem_cache *cachep,
			      gfp_t flags) __assume_slab_alignment __malloc;
#define kmem_cache_alloc(...)			alloc_hooks(kmem_cache_alloc_noprof(__VA_ARGS__))

void *kmem_cache_alloc_lru_noprof(struct kmem_cache *s, struct list_lru *lru,
			    gfp_t gfpflags) __assume_slab_alignment __malloc;
#define kmem_cache_alloc_lru(...)	alloc_hooks(kmem_cache_alloc_lru_noprof(__VA_ARGS__))

/**
 * kmem_cache_charge - memcg charge an already allocated slab memory
 * @objp: address of the slab object to memcg charge
 * @gfpflags: describe the allocation context
 *
 * kmem_cache_charge allows charging a slab object to the current memcg,
 * primarily in cases where charging at allocation time might not be possible
 * because the target memcg is not known (i.e. softirq context)
 *
 * The objp should be pointer returned by the slab allocator functions like
 * kmalloc (with __GFP_ACCOUNT in flags) or kmem_cache_alloc. The memcg charge
 * behavior can be controlled through gfpflags parameter, which affects how the
 * necessary internal metadata can be allocated. Including __GFP_NOFAIL denotes
 * that overcharging is requested instead of failure, but is not applied for the
 * internal metadata allocation.
 *
 * There are several cases where it will return true even if the charging was
 * not done:
 * More specifically:
 *
 * 1. For !CONFIG_MEMCG or cgroup_disable=memory systems.
 * 2. Already charged slab objects.
 * 3. For slab objects from KMALLOC_NORMAL caches - allocated by kmalloc()
 *    without __GFP_ACCOUNT
 * 4. Allocating internal metadata has failed
 *
 * Return: true if charge was successful otherwise false.
 */
bool kmem_cache_charge(void *objp, gfp_t gfpflags);
void kmem_cache_free(struct kmem_cache *s, void *objp);

kmem_buckets *kmem_buckets_create(const char *name, slab_flags_t flags,
				  unsigned int useroffset, unsigned int usersize,
				  void (*ctor)(void *));

/*
 * Bulk allocation and freeing operations. These are accelerated in an
 * allocator specific way to avoid taking locks repeatedly or building
 * metadata structures unnecessarily.
 *
 * Note that interrupts must be enabled when calling these functions.
 */
void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p);

int kmem_cache_alloc_bulk_noprof(struct kmem_cache *s, gfp_t flags, size_t size, void **p);
#define kmem_cache_alloc_bulk(...)	alloc_hooks(kmem_cache_alloc_bulk_noprof(__VA_ARGS__))

static __always_inline void kfree_bulk(size_t size, void **p)
{
	kmem_cache_free_bulk(NULL, size, p);
}

void *kmem_cache_alloc_node_noprof(struct kmem_cache *s, gfp_t flags,
				   int node) __assume_slab_alignment __malloc;
#define kmem_cache_alloc_node(...)	alloc_hooks(kmem_cache_alloc_node_noprof(__VA_ARGS__))

/*
 * These macros allow declaring a kmem_buckets * parameter alongside size, which
 * can be compiled out with CONFIG_SLAB_BUCKETS=n so that a large number of call
 * sites don't have to pass NULL.
 */
#ifdef CONFIG_SLAB_BUCKETS
#define DECL_BUCKET_PARAMS(_size, _b)	size_t (_size), kmem_buckets *(_b)
#define PASS_BUCKET_PARAMS(_size, _b)	(_size), (_b)
#define PASS_BUCKET_PARAM(_b)		(_b)
#else
#define DECL_BUCKET_PARAMS(_size, _b)	size_t (_size)
#define PASS_BUCKET_PARAMS(_size, _b)	(_size)
#define PASS_BUCKET_PARAM(_b)		NULL
#endif

/*
 * The following functions are not to be used directly and are intended only
 * for internal use from kmalloc() and kmalloc_node()
 * with the exception of kunit tests
 */

void *__kmalloc_noprof(size_t size, gfp_t flags)
				__assume_kmalloc_alignment __alloc_size(1);

void *__kmalloc_node_noprof(DECL_BUCKET_PARAMS(size, b), gfp_t flags, int node)
				__assume_kmalloc_alignment __alloc_size(1);

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t flags, size_t size)
				__assume_kmalloc_alignment __alloc_size(3);

void *__kmalloc_cache_node_noprof(struct kmem_cache *s, gfp_t gfpflags,
				  int node, size_t size)
				__assume_kmalloc_alignment __alloc_size(4);

void *__kmalloc_large_noprof(size_t size, gfp_t flags)
				__assume_page_alignment __alloc_size(1);

void *__kmalloc_large_node_noprof(size_t size, gfp_t flags, int node)
				__assume_page_alignment __alloc_size(1);

/**
 * kmalloc - allocate kernel memory
 * @size: how many bytes of memory are required.
 * @flags: describe the allocation context
 *
 * kmalloc is the normal method of allocating memory
 * for objects smaller than page size in the kernel.
 *
 * The allocated object address is aligned to at least ARCH_KMALLOC_MINALIGN
 * bytes. For @size of power of two bytes, the alignment is also guaranteed
 * to be at least to the size. For other sizes, the alignment is guaranteed to
 * be at least the largest power-of-two divisor of @size.
 *
 * The @flags argument may be one of the GFP flags defined at
 * include/linux/gfp_types.h and described at
 * :ref:`Documentation/core-api/mm-api.rst <mm-api-gfp-flags>`
 *
 * The recommended usage of the @flags is described at
 * :ref:`Documentation/core-api/memory-allocation.rst <memory_allocation>`
 *
 * Below is a brief outline of the most useful GFP flags
 *
 * %GFP_KERNEL
 *	Allocate normal kernel ram. May sleep.
 *
 * %GFP_NOWAIT
 *	Allocation will not sleep.
 *
 * %GFP_ATOMIC
 *	Allocation will not sleep.  May use emergency pools.
 *
 * Also it is possible to set different flags by OR'ing
 * in one or more of the following additional @flags:
 *
 * %__GFP_ZERO
 *	Zero the allocated memory before returning. Also see kzalloc().
 *
 * %__GFP_HIGH
 *	This allocation has high priority and may use emergency pools.
 *
 * %__GFP_NOFAIL
 *	Indicate that this allocation is in no way allowed to fail
 *	(think twice before using).
 *
 * %__GFP_NORETRY
 *	If memory is not immediately available,
 *	then give up at once.
 *
 * %__GFP_NOWARN
 *	If allocation fails, don't issue any warnings.
 *
 * %__GFP_RETRY_MAYFAIL
 *	Try really hard to succeed the allocation but fail
 *	eventually.
 */
static __always_inline __alloc_size(1) void *kmalloc_noprof(size_t size, gfp_t flags)
{
	if (__builtin_constant_p(size) && size) {
		unsigned int index;

		if (size > KMALLOC_MAX_CACHE_SIZE)
			return __kmalloc_large_noprof(size, flags);

		index = kmalloc_index(size);
		return __kmalloc_cache_noprof(
				kmalloc_caches[kmalloc_type(flags, _RET_IP_)][index],
				flags, size);
	}
	return __kmalloc_noprof(size, flags);
}
#define kmalloc(...)				alloc_hooks(kmalloc_noprof(__VA_ARGS__))

#define kmem_buckets_alloc(_b, _size, _flags)	\
	alloc_hooks(__kmalloc_node_noprof(PASS_BUCKET_PARAMS(_size, _b), _flags, NUMA_NO_NODE))

#define kmem_buckets_alloc_track_caller(_b, _size, _flags)	\
	alloc_hooks(__kmalloc_node_track_caller_noprof(PASS_BUCKET_PARAMS(_size, _b), _flags, NUMA_NO_NODE, _RET_IP_))

static __always_inline __alloc_size(1) void *kmalloc_node_noprof(size_t size, gfp_t flags, int node)
{
	if (__builtin_constant_p(size) && size) {
		unsigned int index;

		if (size > KMALLOC_MAX_CACHE_SIZE)
			return __kmalloc_large_node_noprof(size, flags, node);

		index = kmalloc_index(size);
		return __kmalloc_cache_node_noprof(
				kmalloc_caches[kmalloc_type(flags, _RET_IP_)][index],
				flags, node, size);
	}
	return __kmalloc_node_noprof(PASS_BUCKET_PARAMS(size, NULL), flags, node);
}
#define kmalloc_node(...)			alloc_hooks(kmalloc_node_noprof(__VA_ARGS__))

/**
 * kmalloc_array - allocate memory for an array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline __alloc_size(1, 2) void *kmalloc_array_noprof(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return kmalloc_noprof(bytes, flags);
}
#define kmalloc_array(...)			alloc_hooks(kmalloc_array_noprof(__VA_ARGS__))

/**
 * krealloc_array - reallocate memory for an array.
 * @p: pointer to the memory chunk to reallocate
 * @new_n: new number of elements to alloc
 * @new_size: new size of a single member of the array
 * @flags: the type of memory to allocate (see kmalloc)
 *
 * If __GFP_ZERO logic is requested, callers must ensure that, starting with the
 * initial memory allocation, every subsequent call to this API for the same
 * memory allocation is flagged with __GFP_ZERO. Otherwise, it is possible that
 * __GFP_ZERO is not fully honored by this API.
 *
 * See krealloc_noprof() for further details.
 *
 * In any case, the contents of the object pointed to are preserved up to the
 * lesser of the new and old sizes.
 */
static inline __realloc_size(2, 3) void * __must_check krealloc_array_noprof(void *p,
								       size_t new_n,
								       size_t new_size,
								       gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(new_n, new_size, &bytes)))
		return NULL;

	return krealloc_noprof(p, bytes, flags);
}
#define krealloc_array(...)			alloc_hooks(krealloc_array_noprof(__VA_ARGS__))

/**
 * kcalloc - allocate memory for an array. The memory is set to zero.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
#define kcalloc(n, size, flags)		kmalloc_array(n, size, (flags) | __GFP_ZERO)

void *__kmalloc_node_track_caller_noprof(DECL_BUCKET_PARAMS(size, b), gfp_t flags, int node,
					 unsigned long caller) __alloc_size(1);
#define kmalloc_node_track_caller_noprof(size, flags, node, caller) \
	__kmalloc_node_track_caller_noprof(PASS_BUCKET_PARAMS(size, NULL), flags, node, caller)
#define kmalloc_node_track_caller(...)		\
	alloc_hooks(kmalloc_node_track_caller_noprof(__VA_ARGS__, _RET_IP_))

/*
 * kmalloc_track_caller is a special version of kmalloc that records the
 * calling function of the routine calling it for slab leak tracking instead
 * of just the calling function (confusing, eh?).
 * It's useful when the call to kmalloc comes from a widely-used standard
 * allocator where we care about the real place the memory allocation
 * request comes from.
 */
#define kmalloc_track_caller(...)		kmalloc_node_track_caller(__VA_ARGS__, NUMA_NO_NODE)

#define kmalloc_track_caller_noprof(...)	\
		kmalloc_node_track_caller_noprof(__VA_ARGS__, NUMA_NO_NODE, _RET_IP_)

static inline __alloc_size(1, 2) void *kmalloc_array_node_noprof(size_t n, size_t size, gfp_t flags,
							  int node)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	if (__builtin_constant_p(n) && __builtin_constant_p(size))
		return kmalloc_node_noprof(bytes, flags, node);
	return __kmalloc_node_noprof(PASS_BUCKET_PARAMS(bytes, NULL), flags, node);
}
#define kmalloc_array_node(...)			alloc_hooks(kmalloc_array_node_noprof(__VA_ARGS__))

#define kcalloc_node(_n, _size, _flags, _node)	\
	kmalloc_array_node(_n, _size, (_flags) | __GFP_ZERO, _node)

/*
 * Shortcuts
 */
#define kmem_cache_zalloc(_k, _flags)		kmem_cache_alloc(_k, (_flags)|__GFP_ZERO)

/**
 * kzalloc - allocate memory. The memory is set to zero.
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline __alloc_size(1) void *kzalloc_noprof(size_t size, gfp_t flags)
{
	return kmalloc_noprof(size, flags | __GFP_ZERO);
}
#define kzalloc(...)				alloc_hooks(kzalloc_noprof(__VA_ARGS__))
#define kzalloc_node(_size, _flags, _node)	kmalloc_node(_size, (_flags)|__GFP_ZERO, _node)

void *__kvmalloc_node_noprof(DECL_BUCKET_PARAMS(size, b), gfp_t flags, int node) __alloc_size(1);
#define kvmalloc_node_noprof(size, flags, node)	\
	__kvmalloc_node_noprof(PASS_BUCKET_PARAMS(size, NULL), flags, node)
#define kvmalloc_node(...)			alloc_hooks(kvmalloc_node_noprof(__VA_ARGS__))

#define kvmalloc(_size, _flags)			kvmalloc_node(_size, _flags, NUMA_NO_NODE)
#define kvmalloc_noprof(_size, _flags)		kvmalloc_node_noprof(_size, _flags, NUMA_NO_NODE)
#define kvzalloc(_size, _flags)			kvmalloc(_size, (_flags)|__GFP_ZERO)

#define kvzalloc_node(_size, _flags, _node)	kvmalloc_node(_size, (_flags)|__GFP_ZERO, _node)
#define kmem_buckets_valloc(_b, _size, _flags)	\
	alloc_hooks(__kvmalloc_node_noprof(PASS_BUCKET_PARAMS(_size, _b), _flags, NUMA_NO_NODE))

static inline __alloc_size(1, 2) void *
kvmalloc_array_node_noprof(size_t n, size_t size, gfp_t flags, int node)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;

	return kvmalloc_node_noprof(bytes, flags, node);
}

#define kvmalloc_array_noprof(...)		kvmalloc_array_node_noprof(__VA_ARGS__, NUMA_NO_NODE)
#define kvcalloc_node_noprof(_n,_s,_f,_node)	kvmalloc_array_node_noprof(_n,_s,(_f)|__GFP_ZERO,_node)
#define kvcalloc_noprof(...)			kvcalloc_node_noprof(__VA_ARGS__, NUMA_NO_NODE)

#define kvmalloc_array(...)			alloc_hooks(kvmalloc_array_noprof(__VA_ARGS__))
#define kvcalloc_node(...)			alloc_hooks(kvcalloc_node_noprof(__VA_ARGS__))
#define kvcalloc(...)				alloc_hooks(kvcalloc_noprof(__VA_ARGS__))

void *kvrealloc_noprof(const void *p, size_t size, gfp_t flags)
		__realloc_size(2);
#define kvrealloc(...)				alloc_hooks(kvrealloc_noprof(__VA_ARGS__))

extern void kvfree(const void *addr);
DEFINE_FREE(kvfree, void *, if (!IS_ERR_OR_NULL(_T)) kvfree(_T))

extern void kvfree_sensitive(const void *addr, size_t len);

unsigned int kmem_cache_size(struct kmem_cache *s);

#ifndef CONFIG_KVFREE_RCU_BATCHED
static inline void kvfree_rcu_barrier(void)
{
	rcu_barrier();
}

static inline void kfree_rcu_scheduler_running(void) { }
#else
void kvfree_rcu_barrier(void);

void kfree_rcu_scheduler_running(void);
#endif

/**
 * kmalloc_size_roundup - Report allocation bucket size for the given size
 *
 * @size: Number of bytes to round up from.
 *
 * This returns the number of bytes that would be available in a kmalloc()
 * allocation of @size bytes. For example, a 126 byte request would be
 * rounded up to the next sized kmalloc bucket, 128 bytes. (This is strictly
 * for the general-purpose kmalloc()-based allocations, and is not for the
 * pre-sized kmem_cache_alloc()-based allocations.)
 *
 * Use this to kmalloc() the full bucket size ahead of time instead of using
 * ksize() to query the size after an allocation.
 */
size_t kmalloc_size_roundup(size_t size);

void __init kmem_cache_init_late(void);
void __init kvfree_rcu_init(void);

#endif	/* _LINUX_SLAB_H */
