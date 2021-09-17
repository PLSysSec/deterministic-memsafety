#include <stdlib.h>

/// Interpreted as in DynamicBoundsInfo in the LLVM pass
struct DynamicBounds {
  void* base;
  void* max;

  DynamicBounds() : base(NULL), max(NULL) {}
  DynamicBounds(void* base, void* max) : base(base), max(max) {}
};

// -- above this line, mirror all changes to dms_interface.h -- //

#include "sanitizer_common/sanitizer_addrhashmap.h"

// AddrHashMap docs recommend a prime for the template arg. It appears to be the
// number of buckets in the hashtable.
// 2311 = 2 * 3 * 5 * 7 * 11 + 1
typedef __sanitizer::AddrHashMap<DynamicBounds, 2311> BoundsMap;
static BoundsMap bounds_map;

namespace __dms {

/// Mark that the dynamic bounds for `ptr` are `base` and `max`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_bounds(void* ptr, void* base, void* max) {
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  *h = DynamicBounds(base, max);
}

/// Get the (previously stored) dynamic bounds for `ptr`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
DynamicBounds __dms_get_bounds(void* ptr) {
  BoundsMap::Handle h(&bounds_map, (__sanitizer::uptr)ptr);
  return *h;
}

} // end namespace
