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

// AddrHashMap docs recommend a prime for the template arg. It's the number of
// buckets in the hashtable.
// 31051 is the prime used by tsan.
typedef __sanitizer::AddrHashMap<DynamicBounds, 31051> BoundsMap;
static BoundsMap bounds_map;

/// Maps global array to the dynamic max for that global array. (The dynamic
/// base is implicitly the map key.)
typedef __sanitizer::AddrHashMap<void*, 2311> GlobalArraySizeMap;
static GlobalArraySizeMap global_array_size_map;

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

/// Copy the current bounds for pointer stored at `src` so that they also apply
/// to the pointer stored at `dst`. (Both `src` and `dst` should have type T**
/// for some T.)
/// Both `src` and `dst` should be UNENCODED pointer values, ie with all upper
/// bits clear.
///
/// Not an error to call this when we haven't previously stored any bounds for
/// `src`. In that case this is a no-op.
void __dms_copy_single_bounds(void* src, void* dst) {
  assert(src != NULL);
  assert(dst != NULL);
  if (src == dst) return;
  void* src_base;
  void* src_max;
  {
    BoundsMap::Handle src_h(&bounds_map, (__sanitizer::uptr)src, /* remove = */ false, /* create = */ false);
    if (!src_h.exists()) return;
    assert(!src_h.created());
    src_base = src_h->base;
    src_max = src_h->max;
    // src_h is dropped, and lock released, at end of scope
  }
  BoundsMap::Handle dst_h(&bounds_map, (__sanitizer::uptr)dst);
  dst_h->base = src_base;
  dst_h->max = src_max;
}

/// Copy the current bounds for every pointer stored in the memory interval
/// [`src`, `src` + `len_bytes`), so that they also apply to the corresponding
/// pointer stored in the interval [`dst`, `dst` + `len_bytes`).
/// Both `src` and `dst` should be UNENCODED pointer values, ie with all upper
/// bits clear.
///
/// This function checks for bounds info for pointers stored at `src`, and every
/// `stride` bytes from `src` within the designated interval.
///
/// Not an error to call this when we haven't previously stored any bounds for
/// any/all of the memory locations in the src interval. In that case this is a
/// no-op.
void __dms_copy_bounds_in_interval(void* src, void* dst, size_t len_bytes, size_t stride) {
  if (len_bytes == 0) return;  // in this case src or dst may be NULL
  assert(src != NULL);
  assert(dst != NULL);
  for (
    __sanitizer::uptr cur_src = (__sanitizer::uptr)src, cur_dst = (__sanitizer::uptr)dst;
    cur_src < (__sanitizer::uptr)src + len_bytes;
    cur_src += stride, cur_dst += stride
  ) {
    __dms_copy_single_bounds((void*)cur_src, (void*)cur_dst);
  }
}

/// Mark that the given global array `arr` has dynamic base `arr` and max `max`.
/// This is used in case the array is declared in another translation unit as
/// e.g. `extern int some_arr[];`. That other translation unit can then dynamically
/// look up the size recorded here by the translation unit that actually has the
/// accurate size for `some_arr`.
void __dms_store_globalarraysize(void* arr, void* max) {
  assert(arr != NULL);
  GlobalArraySizeMap::Handle h(&global_array_size_map, (__sanitizer::uptr)arr);
  *h = max;
}

/// Get the dynamic max for the global array `arr`. (The dynamic base is
/// implicitly `arr` itself.) See ntoes on `__dms_store_globalarraysize()`.
/// The dynamic max is written to the output parameter `max`.
void __dms_get_globalarraysize(void* arr, void** max) {
  assert(arr != NULL);
  assert(max != NULL);
  GlobalArraySizeMap::Handle h(&global_array_size_map, (__sanitizer::uptr)arr);
  if (h.created()) {
    fprintf(stderr, "Global array size lookup failed: no size for the array at address %p\n", arr);
    __sanitizer::ReportErrorSummary("Global array size lookup failure");
    __sanitizer::Abort();
    // Or, we could make this just a warning, like in the BoundsMap case, if necessary.
    //*max = infinite_bounds().max;
    // also store the infinite size in case we look up the same array again
    //*h = infinite_bounds().max;
  } else {
    *max = *h;
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
