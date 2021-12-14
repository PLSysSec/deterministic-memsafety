#include <stdlib.h>
#include <stdint.h>

/// Interpreted as in DynamicBoundsInfo in the LLVM pass
struct DynamicBounds {
  void* base;
  void* max;
  uint64_t infinite; // if nonzero (true), then consider this to be infinite bounds; `base` and `max` may not be valid
    // we use 'uint64_t' just to make the struct layout blindingly obvious and avoid any possible ABI mismatch with the calling code
};

DynamicBounds dynamic_bounds(void* base, void* max);
DynamicBounds infinite_bounds();

// -- above this line, mirror all changes to dms_interface.h -- //

DynamicBounds dynamic_bounds(void* base, void* max) {
  return DynamicBounds { base, max, 0 };
}

DynamicBounds infinite_bounds() {
  return DynamicBounds { NULL, NULL, 1 };
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

/// Mark that the dynamic bounds for `ptr` are `base` and `max`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_bounds(void* ptr, void* base, void* max) {
  if (ptr == NULL) return; // null ptr always has infinite bounds; doesn't need to be stored in hashtable. Even if this call was trying to establish more restrictive bounds for a nullptr, it's safe to extend them to infinite, bc it will trap on dereference anyway, no need for bounds check
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  *h = dynamic_bounds(base, max);
}

/// Mark that the dynamic bounds for `ptr` should be considered infinite.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_infinite_bounds(void* ptr) {
  if (ptr == NULL) return; // null ptr always has infinite bounds; doesn't need to be stored in hashtable
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  *h = infinite_bounds();
}

/// Get the (previously stored) dynamic bounds for `ptr`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
///
/// If this returns true (nonzero), then `ptr` has been marked as infinite bounds.
/// If this returns false (zero), then this writes the base and max to the
/// output parameters `base` and `max`.
char __dms_get_bounds(void* ptr, void** base, void** max) {
  if (ptr == NULL) return 1; // null ptr always has infinite bounds; we never need to bounds-check it
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  if (h.created()) {
    fprintf(stderr, "Bounds lookup failed: no bounds for %p\n", ptr);
    __sanitizer::ReportErrorSummary("Bounds lookup failure");
    __sanitizer::Abort();
  }
  if (h->infinite) {
    return 1;
  } else {
    assert(base && max && "base and max must not be NULL here");
    *base = h->base;
    *max = h->max;
    return 0;
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
