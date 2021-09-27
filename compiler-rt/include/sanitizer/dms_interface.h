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

namespace __dms {

/// Mark that the dynamic bounds for `ptr` are `base` and `max`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_bounds(void* ptr, void* base, void* max);

/// Mark that the dynamic bounds for `ptr` should be considered infinite.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_infinite_bounds(void* ptr);

/// Get the (previously stored) dynamic bounds for `ptr`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
struct DynamicBounds __dms_get_bounds(void* ptr);

} // end namespace
