#include <stdlib.h>
#include <stdint.h>

namespace __dms {

/// Mark that the dynamic bounds for the pointer P stored at location `addr` are
/// `base` and `max`. (This implies that `addr` has type T** for some T.)
/// In the normal case, `base` <= P <= `max`; `addr` is &P.
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
void __dms_store_bounds(void* addr, void* base, void* max);

/// Mark that the dynamic bounds for the pointer P stored at location `addr`
/// should be considered infinite. (This implies that `addr` has type T** for
/// some T.)
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
void __dms_store_infinite_bounds(void* addr);

/// Get the (previously stored) dynamic bounds for pointer P stored at location
/// `addr`. (This implies that `addr` has type T** for some T.)
/// Of course, `addr` should be an UNENCODED value, ie with all upper bits clear.
/// (P is probably an encoded pointer value, and that's fine.)
///
/// This writes the base and max to the output parameters `base` and `max`.
/// P will be required to satisfy `base` <= P <= `max`.
/// If P has been marked as infinite bounds, then `base` will be 0 and `max`
/// will be 0xFFFFF...
void __dms_get_bounds(void* addr, void** base, void** max);

/// Call this to indicate that a bounds check failed for `ptr`.
/// This function will not return.
__attribute__((noreturn))
void __dms_boundscheckfail(void* ptr);

} // end namespace
