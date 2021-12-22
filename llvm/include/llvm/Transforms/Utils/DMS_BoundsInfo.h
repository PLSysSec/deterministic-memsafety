#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

#include <variant>

extern const llvm::APInt zero;
extern const llvm::APInt minusone;

namespace llvm {

/// Holds the bounds information for a single pointer, if it is known.
/// Unlike the PointerStatus, which can be different at different program
/// points for the same pointer, the BoundsInfo is the same at all program
/// points for a given pointer.
class BoundsInfo final {
public:
  enum Kind {
    // NOTDEFINEDYET means that the pointer has not been defined yet at this
    // program point (at least, to our current knowledge). All pointers are
    // (effectively) initialized to NOTDEFINEDYET at the beginning of the
    // analysis.
    // Whenever it encounters a pointer definition, the DMS pass should also
    // note bounds for the pointer -- at a bare minimum, marking the bounds
    // UNKNOWN instead of NOTDEFINEDYET.
    NOTDEFINEDYET = 0,
    /// Bounds info is not known for this pointer. In the future, dereferencing
    /// these pointers should be a compile-time error - we should know bounds
    /// info for all pointers which are ever dereferenced.
    UNKNOWN,
    /// Bounds info is known statically. Get it with `static_info()`
    STATIC,
    /// Bounds info is known dynamically. Get it with `dynamic_info()`
    DYNAMIC,
    /// Bounds info is known dynamically, and is derived as the merger of the
    /// bounds infos in `.merge_inputs`. Get it with `dynamic_info()`
    DYNAMIC_MERGED,
    /// This pointer should be considered to have infinite bounds in both
    /// directions
    INFINITE,
  };

  Kind get_kind() const {
    return kind;
  }

  const char* pretty_kind() const {
    switch (kind) {
      case NOTDEFINEDYET: return "NOTDEFINEDYET";
      case UNKNOWN: return "UNKNOWN";
      case STATIC: return "STATIC";
      case DYNAMIC: return "DYNAMIC";
      case DYNAMIC_MERGED: return "DYNAMIC_MERGED";
      case INFINITE: return "INFINITE";
      default: llvm_unreachable("Missing BoundsInfo.kind case");
    }
  }

  std::string pretty() const;

  /// Statically known bounds info.
  ///
  /// Suppose the pointer value is P. If the `low_offset` is L and the
  /// `high_offset` is H, that means we know that the allocation extends at
  /// least from (P + L) to (P + H), inclusive.
  /// Notes:
  ///   - `low_offset` and `high_offset` are in bytes.
  ///   - In the common case (where we know P is inbounds) `low_offset` will
  ///     be <= 0 and `high_offset` will be >= 0.
  ///   - Values of 0 in both fields indicates a pointer valid for exactly the
  ///     single byte it points to.
  class StaticBoundsInfo final {
  public:
    APInt low_offset;
    APInt high_offset;

    explicit StaticBoundsInfo(APInt low_offset, APInt high_offset)
      : low_offset(low_offset), high_offset(high_offset) {}
    explicit StaticBoundsInfo(uint64_t low_offset, uint64_t high_offset)
      : low_offset(APInt(/* numBits = */ 64, /* val = */ low_offset)),
        high_offset(APInt(/* numBits = */ 64, /* val = */ high_offset)) {}
    StaticBoundsInfo() : low_offset(zero), high_offset(minusone) {}

    bool operator==(const StaticBoundsInfo& other) const {
      return (low_offset == other.low_offset && high_offset == other.high_offset);
    }
    bool operator!=(const StaticBoundsInfo& other) const {
      return !(*this == other);
    }

    /// `cur_ptr`: the pointer value for which these static bounds apply.
    ///
    /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
    ///
    /// Returns the "base" (pointer to the first byte) of the allocation, as an
    /// LLVM `Value` of type `i8*`.
    Value* base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(cur_ptr, low_offset);
    }

    /// `cur_ptr`: the pointer value for which these static bounds apply.
    ///
    /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
    ///
    /// Returns the "max" (pointer to the last byte) of the allocation, as an
    /// LLVM `Value` of type `i8*`.
    Value* max_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(cur_ptr, high_offset);
    }

    /// Do the current bounds fail? Meaning, if we were to perform a memory
    /// access (of size `access_bytes`) with a pointer with these bounds, would
    /// the SW bounds check fail?
    ///
    /// Since the bounds are known statically, we can answer this statically and
    /// without inserting any dynamic instructions
    bool fails(unsigned access_bytes) const {
      assert(access_bytes > 0);
      if (low_offset.isStrictlyPositive()) {
        // invalid pointer: too low
        return true;
      } else if (high_offset.slt(access_bytes - 1)) {
        // invalid pointer: too high
        return true;
      } else {
        return false;
      }
    }
  };

  /// Represents a pointer value as an LLVM pointer, with an optional
  /// constant offset.
  ///
  /// The reason we represent it this way, rather than directly as a
  /// LLVM Value (eg an LLVM GEP on a cast of `ptr`), is that this
  /// representation is easier to compare for equality.
  struct PointerWithOffset {
    /// Pointer value itself.
    /// This can be a `Value` of any pointer type.
    Value* ptr;
    /// Offset, in bytes.
    /// This can be positive, negative, or zero.
    APInt offset;

    /// Get the pointer value as an LLVM `Value` of type `i8*`.
    Value* as_llvm_value(DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(ptr, offset);
    }

    PointerWithOffset() : ptr(NULL), offset(zero) {}
    PointerWithOffset(Value* ptr) : ptr(ptr), offset(zero) {}
    PointerWithOffset(Value* ptr, APInt offset) : ptr(ptr), offset(offset) {}
    PointerWithOffset(Value* ptr, uint64_t offset) : ptr(ptr), offset(APInt(/* bits = */ 64, /* val = */ offset)) {}

    bool operator==(const PointerWithOffset& other) const {
      return (ptr == other.ptr && offset == other.offset);
    }
    bool operator!=(const PointerWithOffset& other) const {
      return !(*this == other);
    }
  };

  /// Dynamically known bounds info
  ///
  /// This is computed lazily: it's either actual dynamic bounds info, or just
  /// the information needed to (lazily) compute it if/when needed.
  ///
  /// Again this should be a discriminated union, but we can't use C++17 here
  struct DynamicBoundsInfo {
    /// Gives the "base", i.e., pointer to the first byte of the allocation.
    /// Forces the bounds info to be computed if it hasn't been already.
    const PointerWithOffset& getBase() const {
      // we "cheat" the const here by calling the non-const `force()`
      ((DynamicBoundsInfo*)this)->force();
      return std::get<Info>(data).base;
    }

    /// Gives the "max", i.e., pointer to the last byte of the allocation.
    /// Forces the bounds info to be computed if it hasn't been already.
    const PointerWithOffset& getMax() const {
      // we "cheat" the const here by calling the non-const `force()`
      ((DynamicBoundsInfo*)this)->force();
      return std::get<Info>(data).max;
    }

    void force() {
      if (LazyInfo* lazy = std::get_if<LazyInfo>(&data)) {
        data = lazy->force();
      } else if (LazyGlobalArraySize* lazy = std::get_if<LazyGlobalArraySize>(&data)) {
        data = lazy->force();
      }
    }

  private:
    /// Actual dynamic bounds info
    struct Info {
      /// Pointer to the first byte of the allocation
      PointerWithOffset base;
      /// Pointer to the last byte of the allocation
      PointerWithOffset max;

      explicit Info(Value* base, Value* max)
        : base(PointerWithOffset(base)), max(PointerWithOffset(max)) {}
      explicit Info(PointerWithOffset base, PointerWithOffset max)
        : base(base), max(max) {}
      Info() : base(PointerWithOffset()), max(PointerWithOffset()) {}

      bool operator==(const Info& other) const {
        return (base == other.base && max == other.max);
      }
      bool operator!=(const Info& other) const {
        return !(*this == other);
      }
    };

    /// Information needed to (lazily) compute the `Info` if/when needed
    struct LazyInfo {
      /// Dynamic info for the pointer at which address (this should be a _decoded_ ptr)
      Value* addr;
      /// Dynamic info for which loaded ptr (this should be a _decoded_ ptr)
      /// Except for decoding, `loaded_ptr` should have been produced by loading
      /// the above `addr`
      Instruction* loaded_ptr;
      /// Reference to the `added_insts` where we note any instructions added
      /// for bounds purposes. See notes on `added_insts` in `DMSAnalysis`
      ///
      /// This is allowed to be `NULL` only if we never `force()` this
      /// `LazyInfo` (for instance if `lazy_type` is `NOTLAZY`)
      DenseSet<const Instruction*>* added_insts;

      explicit LazyInfo(Value* addr, Instruction* loaded_ptr, DenseSet<const Instruction*>& added_insts)
        : addr(addr), loaded_ptr(loaded_ptr), added_insts(&added_insts) {}
      LazyInfo() : addr(NULL), loaded_ptr(NULL), added_insts(NULL) {}

      Info force();

      bool operator==(const LazyInfo& other) const {
        // don't need to check that added_insts are equal
        return (addr == other.addr && loaded_ptr == other.loaded_ptr);
      }
      bool operator!=(const LazyInfo& other) const {
        return !(*this == other);
      }
    };

    /// Information needed to (lazily) compute the `Info` representing bounds
    /// for a global array. This is only used if the global array size is 0 in
    /// this compilation unit, eg because of a declaration like
    /// `extern int some_arr[];`. In that case we (lazily) do the dynamic lookup
    /// to get the actual global array size from our runtime; see notes there.
    struct LazyGlobalArraySize {
      /// Global array in question
      ///
      /// This is allowed to be `NULL` only if we never `force()` this
      /// `LazyGlobalArraySize` (for instance if `lazy_type` is `NOTLAZY`)
      GlobalValue* arr;
      /// `Function` which we should insert dynamic instructions in, in the
      /// event that we need to call `force()` to actually do the dynamic
      /// lookup.
      ///
      /// This is allowed to be `NULL` only if we never `force()` this
      /// `LazyGlobalArraySize` (for instance if `lazy_type` is `NOTLAZY`)
      Function* insertion_func;
      /// Reference to the `added_insts` where we note any instructions added
      /// for bounds purposes. See notes on `added_insts` in `DMSAnalysis`
      ///
      /// This is allowed to be `NULL` only if we never `force()` this
      /// `LazyGlobalArraySize` (for instance if `lazy_type` is `NOTLAZY`)
      DenseSet<const Instruction*>* added_insts;

      explicit LazyGlobalArraySize(GlobalValue& arr, Function& insertion_func, DenseSet<const Instruction*>& added_insts)
        : arr(&arr), insertion_func(&insertion_func), added_insts(&added_insts) {}
      LazyGlobalArraySize() : arr(NULL), insertion_func(NULL), added_insts(NULL) {}

      Info force();

      bool operator==(const LazyGlobalArraySize& other) const {
        // don't need to check that added_insts are equal
        return (arr == other.arr);
      }
      bool operator!=(const LazyGlobalArraySize& other) const {
        return !(*this == other);
      }
    };

    /// Holds the actual data: either a real `Info`, or one of two ways to
    /// lazily compute it when it is needed
    std::variant<Info, LazyInfo, LazyGlobalArraySize> data;

  public:
    explicit DynamicBoundsInfo(Value* base, Value* max)
      : data(Info(base, max)) {}
    explicit DynamicBoundsInfo(PointerWithOffset base, PointerWithOffset max)
      : data(Info(base, max)) {}
    DynamicBoundsInfo() : data(Info()) {}

    /// `addr`: Address of the pointer for which we need dynamic info (this
    /// should be a _decoded_ ptr)
    ///
    /// `loaded_ptr`: Pointer value for which we need dynamic info (this should
    /// be a _decoded_ ptr)
    /// Except for decoding, `loaded_ptr` should have been produced by loading
    /// the above `addr`
    ///
    /// `added_insts`: Reference to the `added_insts` where we note any
    /// instructions added for bounds purposes. See notes on `added_insts` in
    /// `DMSAnalysis`
    static DynamicBoundsInfo LazyDynamicBoundsInfo(Value* addr, Instruction* loaded_ptr, DenseSet<const Instruction*>& added_insts) {
      DynamicBoundsInfo dyninfo;
      dyninfo.data = LazyInfo(addr, loaded_ptr, added_insts);
      return dyninfo;
    }

    /// This is used to dynamically lookup the size of a global array. We only
    /// do this if the global array size is 0 in this compilation unit, eg
    /// because of a declaration like `extern int some_arr[];`.
    static DynamicBoundsInfo LazyDynamicGlobalArrayBounds(GlobalValue& arr, Function& insertion_func, DenseSet<const Instruction*>& added_insts) {
      DynamicBoundsInfo dyninfo;
      dyninfo.data = LazyGlobalArraySize(arr, insertion_func, added_insts);
      return dyninfo;
    }

    bool operator==(const DynamicBoundsInfo& other) const {
      return (data == other.data);
    }
    bool operator!=(const DynamicBoundsInfo& other) const {
      return !(*this == other);
    }
  };

  /// Get the StaticBoundsInfo, or NULL if not `is_static()`
  const StaticBoundsInfo* static_info() const {
    if (is_static()) {
      return &info.static_info;
    } else {
      return NULL;
    }
  }

  /// Get the DynamicBoundsInfo, or NULL if not `is_dynamic()`
  const DynamicBoundsInfo* dynamic_info() const {
    if (is_dynamic()) {
      return &info.dynamic_info;
    } else {
      return NULL;
    }
  }

  /// Helper, returns `true` if the kind is `STATIC`.
  /// If this returns `true`, `static_info()` will return non-NULL
  bool is_static() const {
    return (kind == STATIC);
  }

  /// Helper, returns `true` if the kind is `DYNAMIC` or `DYNAMIC_MERGED`.
  /// In either case, if this returns `true`, `dynamic_info()` will return
  /// non-NULL
  bool is_dynamic() const {
    return (kind == DYNAMIC || kind == DYNAMIC_MERGED);
  }

  /// Only valid if kind is `DYNAMIC_MERGED`.
  /// Elements in this are new()'d - they must be delete()'d.
  /// Elements in this will never be of kind `DYNAMIC_MERGED` themselves.
  SmallVector<BoundsInfo*, 4> merge_inputs;

  /// Construct a BoundsInfo with the given `StaticBoundsInfo`
  explicit BoundsInfo(StaticBoundsInfo static_info) :
    kind(STATIC), info(static_info) {}

  /// Construct a BoundsInfo with the given static `low_offset` and
  /// `high_offset`
  static BoundsInfo static_bounds(APInt low_offset, APInt high_offset) {
    return BoundsInfo(StaticBoundsInfo(low_offset, high_offset));
  }
  /// Construct a BoundsInfo with the given static `low_offset` and
  /// `high_offset`
  static BoundsInfo static_bounds(uint64_t low_offset, uint64_t high_offset) {
    return BoundsInfo(StaticBoundsInfo(low_offset, high_offset));
  }

  /// Construct a BoundsInfo with the given `DynamicBoundsInfo`
  explicit BoundsInfo(DynamicBoundsInfo dynamic_info) :
    kind(DYNAMIC), info(dynamic_info) {}

  /// Construct a BoundsInfo with the given dynamic `base` and `max`
  static BoundsInfo dynamic_bounds(Value* base, Value* max) {
    return BoundsInfo(DynamicBoundsInfo(base, max));
  }

  /// Construct a BoundsInfo with the given dynamic `base` and `max`
  static BoundsInfo dynamic_bounds(PointerWithOffset base, PointerWithOffset max) {
    return BoundsInfo(DynamicBoundsInfo(base, max));
  }

  /// Construct a BoundsInfo with infinite bounds in both directions
  static BoundsInfo infinite() {
    BoundsInfo ret;
    ret.kind = INFINITE;
    return ret;
  }

  /// Construct a BoundsInfo with unknown bounds
  static BoundsInfo unknown() {
    BoundsInfo ret;
    ret.kind = UNKNOWN;
    return ret;
  }

  /// Construct a BoundsInfo with NOTDEFINEDYET bounds. This is the default when
  /// constructing a BoundsInfo; for instance, DenseMap.lookup() will use this
  /// constructor to construct a BoundsInfo if the key is not found in the map.
  explicit BoundsInfo() : kind(NOTDEFINEDYET), info(StaticBoundsInfo()) {}
  static BoundsInfo notdefinedyet() {
    return BoundsInfo();
  }

  BoundsInfo(const BoundsInfo& other) :
    kind(other.kind), info(other.info) {
    // separately new() for each BoundsInfo in merge_inputs.
    // That way, if `other` is destructed before this new copy,
    // it doesn't delete() the merge_inputs we're using
    for (BoundsInfo* binfo : other.merge_inputs) {
      merge_inputs.push_back(new BoundsInfo(*binfo));
    }
  }
  // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
  // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
  /*friend*/ static void swap(BoundsInfo& A, BoundsInfo& B) noexcept {
    std::swap(A.kind, B.kind);
    std::swap(A.info, B.info);
    std::swap(A.merge_inputs, B.merge_inputs);
  }
  BoundsInfo(BoundsInfo&& other) noexcept : BoundsInfo() {
    swap(*this, other);
  }
  BoundsInfo& operator=(BoundsInfo rhs) noexcept {
    swap(*this, rhs);
    return *this;
  }
  ~BoundsInfo() {
    for (BoundsInfo* binfo : merge_inputs) {
      delete binfo;
    }
  }

  // operator== and operator!= intentionally ignore the .merge_inputs field
  // here
  bool operator==(const BoundsInfo& other) const {
    if (kind != other.kind) return false;
    switch (kind) {
      case NOTDEFINEDYET:
      case UNKNOWN:
      case INFINITE:
        return true;
      case STATIC:
        return (info.static_info == other.info.static_info);
      case DYNAMIC:
      case DYNAMIC_MERGED:
        return (info.dynamic_info == other.info.dynamic_info);
      default:
        llvm_unreachable("Missing BoundsInfo.kind case");
    }
  }
  bool operator!=(const BoundsInfo& other) const {
    return !(*this == other);
  }

  /// `cur_ptr`: the pointer value for which these bounds apply.
  ///
  /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the "base" (pointer to the first byte) of the allocation,
  /// as an LLVM `Value` of type `i8*`.
  /// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
  /// The `kind` should not be NOTDEFINEDYET.
  Value* base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// `cur_ptr`: the pointer value for which these bounds apply.
  ///
  /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the "max" (pointer to the last byte) of the allocation,
  /// as an LLVM `Value` of type `i8*`.
  /// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
  /// The `kind` should not be NOTDEFINEDYET.
  Value* max_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// `cur_ptr` is the pointer which these bounds are for.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary.
  static BoundsInfo merge(
    const BoundsInfo& A,
    const BoundsInfo& B,
    Value* cur_ptr,
    DMSIRBuilder& Builder
  );

  /// Insert dynamic instructions to store this bounds info.
  ///
  /// `ptr` is the pointer this bounds info applies to; call it P. `addr` is &P.
  /// Both `ptr` and `addr` need to be _decoded_ pointer values.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the Call instruction if one was inserted, or else NULL
  CallInst* store_dynamic(Value* addr, Value* ptr, DMSIRBuilder& Builder) const;

  /// Get a `DynamicBoundsInfo` representing dynamic bounds for the pointer
  /// `loaded_ptr`, which should have been loaded from the given `addr`. (I.e.,
  /// `addr == &loaded_ptr`.)
  /// (Both `addr` and `loaded_ptr` should be _decoded_ pointer values.)
  ///
  /// Computes the actual bounds lazily, i.e., does not insert any dynamic
  /// instructions unless/until this `DynamicBoundsInfo` is actually needed
  /// for a bounds check.
  ///
  /// Bounds info for this `addr` should have been previously stored with
  /// `store_dynamic()`.
  static DynamicBoundsInfo dynamic_bounds_for_ptr(
    Value* addr,
    Instruction* loaded_ptr,
    DenseSet<const Instruction*>& added_insts
  ) {
    return DynamicBoundsInfo::LazyDynamicBoundsInfo(addr, loaded_ptr, added_insts);
  }

  /// Get a `DynamicBoundsInfo` representing dynamic bounds for the global array
  /// `arr`. If dynamic instructions need to be inserted, insert them at the top
  /// of `insertion_func`.
  ///
  /// Computes the actual bounds lazily, i.e., does not insert any dynamic
  /// instructions unless/until this `DynamicBoundsInfo` is actually needed for
  /// a bounds check.
  ///
  /// This is only used for global arrays which have size 0 in this compilation
  /// unit (eg because of a declaration like `extern int some_arr[];`), in which
  /// case we need a dynamic lookup to find the actual bounds. For all other
  /// global arrays we know the bounds statically.
  static DynamicBoundsInfo dynamic_bounds_for_global_array(
    GlobalValue& arr,
    Function& insertion_func,
    DenseSet<const Instruction*>& added_insts
  ) {
    return DynamicBoundsInfo::LazyDynamicGlobalArrayBounds(arr, insertion_func, added_insts);
  }

private:
  /// This should really be a union.  But C++ complains hard about implicitly
  /// deleted copy constructors, implicitly deleted assignment operators, etc
  /// and I'm not C++ enough to be able to fix it
  struct StaticOrDynamic {
    StaticBoundsInfo static_info;
    DynamicBoundsInfo dynamic_info;

    StaticOrDynamic(StaticBoundsInfo static_info) : static_info(static_info) {}
    StaticOrDynamic(DynamicBoundsInfo dynamic_info) : dynamic_info(dynamic_info) {}
  };

  Kind kind;
  StaticOrDynamic info;

  /// `cur_ptr` is the pointer which these bounds are for.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
  static BoundsInfo merge_static_dynamic(
    const StaticBoundsInfo& static_info,
    const DynamicBoundsInfo& dynamic_info,
    Value* cur_ptr,
    DMSIRBuilder& Builder
  );

  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
  static BoundsInfo merge_dynamic_dynamic(
    const DynamicBoundsInfo& a_info,
    const DynamicBoundsInfo& b_info,
    DMSIRBuilder& Builder
  );
};

} // end namespace
