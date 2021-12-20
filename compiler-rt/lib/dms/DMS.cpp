#include <stdlib.h>
#include <stdint.h>

/// Interpreted as in DynamicBoundsInfo in the LLVM pass
struct DynamicBounds {
  void* base;  // NULL for infinite bounds
  void* max;   // (void*)(-1) for infinite bounds
};

DynamicBounds dynamic_bounds(void* base, void* max) {
  return DynamicBounds { base, max };
}

DynamicBounds infinite_bounds() {
  return DynamicBounds { NULL, (void*)(-1) };
}

#include "sanitizer_common/sanitizer_addrhashmap.h"
#include <stdio.h>
#include <assert.h>

// AddrHashMap docs recommend a prime for the template arg. It appears to be the
// number of buckets in the hashtable.
// 2311 = 2 * 3 * 5 * 7 * 11 + 1
typedef __sanitizer::AddrHashMap<DynamicBounds, 2311> BoundsMap;
static BoundsMap bounds_map;

namespace __dms {

/// Mark that the dynamic bounds for the pointer P stored at location `addr` are
/// `base` and `max`. (This implies that `addr` has type T** for some T.)
/// In the normal case, `base` <= P <= `max`; `addr` is &P.
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
void __dms_store_bounds(void* addr, void* base, void* max) {
  assert(addr != NULL);
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)addr);
  *h = dynamic_bounds(base, max);
}

/// Mark that the dynamic bounds for the pointer P stored at location `addr`
/// should be considered infinite. (This implies that `addr` has type T** for
/// some T.)
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
void __dms_store_infinite_bounds(void* addr) {
  assert(addr != NULL);
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)addr);
  *h = infinite_bounds();
}

/// Get the (previously stored) dynamic bounds for pointer P stored at location
/// `addr`. (This implies that `addr` has type T** for some T.)
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
///
/// This writes the base and max to the output parameters `base` and `max`.
/// P will be required to satisfy `base` <= P <= `max`.
/// If P has been marked as infinite bounds, then `base` will be 0 and `max`
/// will be 0xFFFFF...
void __dms_get_bounds(void* addr, void** base, void** max) {
  assert(addr != NULL);
  assert(base && max && "base and max must not be NULL here");
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)addr);
  if (h.created()) {
    #ifndef NDEBUG
    fprintf(stderr, "Bounds lookup failed: no bounds for the pointer at address %p\n", addr);
    #endif
    // but this is not a hard error.
    // now that we do the lookup based on `addr` and not P itself, the risk is a
    // lot lower from just letting untracked pointers be considered infinite
    // bounds.
    //__sanitizer::ReportErrorSummary("Bounds lookup failure");
    //__sanitizer::Abort();
    DynamicBounds inf = infinite_bounds();
    *base = inf.base;
    *max = inf.max;
    // also store these infinite bounds in case we look up the same pointer again
    h->base = inf.base;
    h->max = inf.max;
  } else {
    *base = h->base;
    *max = h->max;
  }
}

/// Call this to indicate that a bounds check failed for `ptr`.
/// This function will not return.
__attribute__((noreturn))
void __dms_boundscheckfail(void* ptr) {
  fprintf(stderr, "Aborting due to bounds check failure for %p\n", ptr);
  __sanitizer::ReportErrorSummary("Bounds check failure");
  __sanitizer::Abort();
}

} // end namespace
