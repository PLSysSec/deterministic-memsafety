#include <stdlib.h>

/// Interpreted as in DynamicBoundsInfo in the LLVM pass
struct DynamicBounds {
  void* base;
  void* max;

  DynamicBounds() : base(NULL), max(NULL) {}
  DynamicBounds(void* base, void* max) : base(base), max(max) {}
};

namespace __dms {

/// Mark that the dynamic bounds for `ptr` are `base` and `max`
void __dms_store_bounds(void* ptr, void* base, void* max);

/// Get the (previously stored) dynamic bounds for `ptr`
struct DynamicBounds __dms_get_bounds(void* ptr);

} // end namespace
