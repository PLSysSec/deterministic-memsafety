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

namespace __dms {

/// Mark that the dynamic bounds for `ptr` are `base` and `max`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_bounds(void* ptr, void* base, void* max);

/// Mark that the dynamic bounds for `ptr` should be considered infinite.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
void __dms_store_infinite_bounds(void* ptr);

/// Get the (previously stored) dynamic bounds for `ptr`.
/// `ptr` should be an UNENCODED value, ie with all upper bits clear.
///
/// If this returns true (nonzero), then `ptr` has been marked as infinite bounds.
/// If this returns false (zero), then this writes the base and max to the
/// output parameters `base` and `max`.
char __dms_get_bounds(void* ptr, void** base, void** bound);

/// Call this to indicate that a bounds check failed for `ptr`.
/// This function will not return.
__attribute__((noreturn))
void __dms_boundscheckfail(void* ptr);

} // end namespace
