#include <stdlib.h>

/// Interpreted as in DynamicBoundsInfo in the LLVM pass
struct DynamicBounds {
  bool infinite; // if `true`, then consider this to be infinite bounds; `base` and `max` may not be valid
  void* base;
  void* max;

  DynamicBounds() : infinite(false), base(NULL), max(NULL) {}
  DynamicBounds(void* base, void* max) : infinite(false), base(base), max(max) {}
  DynamicBounds(bool infinite) : infinite(infinite), base(NULL), max(NULL) {}
};

// -- above this line, mirror all changes to dms_interface.h -- //

#include "sanitizer_common/sanitizer_addrhashmap.h"
#include <stdio.h>

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
  *h = DynamicBounds(base, max);
}

/// Mark that the dynamic bounds for `ptr` should be considered infinite.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_infinite_bounds(void* ptr) {
  if (ptr == NULL) return; // null ptr always has infinite bounds; doesn't need to be stored in hashtable
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  *h = DynamicBounds(true);
}

/// Get the (previously stored) dynamic bounds for `ptr`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
DynamicBounds __dms_get_bounds(void* ptr) {
  if (ptr == NULL) return DynamicBounds(true); // null ptr always has infinite bounds; we never need to bounds-check it
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  return *h;
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
