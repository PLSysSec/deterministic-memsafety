#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

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
    /// Bounds info is not known for this pointer. Dereferencing this
    /// pointer is a compile-time error - we should know bounds info for
    /// all pointers which are ever dereferenced.
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
    /// Returns the "base" (minimum inbounds pointer value) of the allocation,
    /// as an LLVM `Value` of type `i8*`.
    Value* base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(cur_ptr, low_offset);
    }

    /// `cur_ptr`: the pointer value for which these static bounds apply.
    ///
    /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
    ///
    /// Returns the "max" (maximum inbounds pointer value) of the allocation,
    /// as an LLVM `Value` of type `i8*`.
    Value* max_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(cur_ptr, high_offset);
    }

    /// Do the current bounds fail? Meaning, if we were to insert a SW bounds
    /// check for this `StaticBoundsInfo` right now, would the check fail?
    ///
    /// Since the bounds are known statically, we can give an answer to this
    /// statically and without inserting any dynamic instructions
    bool fails() const {
      if (low_offset.isStrictlyPositive()) {
        // invalid pointer: too low
        return true;
      } else if (high_offset.isNegative()) {
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
  struct DynamicBoundsInfo {
    /// Pointer value representing the beginning of the allocation
    /// (i.e., the minimum valid pointer value)
    PointerWithOffset base;
    /// Pointer value representing the end of the allocation
    /// (i.e., the maximum valid pointer value)
    PointerWithOffset max;

    explicit DynamicBoundsInfo(Value* base, Value* max)
      : base(PointerWithOffset(base)), max(PointerWithOffset(max)) {}
    explicit DynamicBoundsInfo(PointerWithOffset base, PointerWithOffset max)
      : base(base), max(max) {}
    DynamicBoundsInfo() : base(PointerWithOffset()), max(PointerWithOffset()) {}

    bool operator==(const DynamicBoundsInfo& other) const {
      return (base == other.base && max == other.max);
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
  /// Returns the "base" (minimum inbounds pointer value) of the allocation,
  /// as an LLVM `Value` of type `i8*`.
  /// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
  /// The `kind` should not be NOTDEFINEDYET.
  Value* base_as_llvm_value(Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// `cur_ptr`: the pointer value for which these bounds apply.
  ///
  /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the "max" (maximum inbounds pointer value) of the allocation,
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
  /// `cur_ptr` is the pointer which these bounds are for.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the Call instruction if one was inserted, or else NULL
  CallInst* store_dynamic(Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// Insert dynamic instructions to load bounds info for the given `ptr`.
  /// Bounds info for this `ptr` should have been previously stored with
  /// `store_dynamic()`.
  ///
  /// Insert dynamic instructions using the given `DMSIRBuilder`.
  static DynamicBoundsInfo load_dynamic(Value* ptr, DMSIRBuilder& Builder);

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

/// Holds the bounds information for all pointers in the function.
class BoundsInfos final {
private:
  /// Maps a pointer to its bounds info, if we know anything about its bounds
  /// info.
  /// For pointers not appearing in this map, we don't know anything about their
  /// bounds.
  DenseMap<const Value*, BoundsInfo> map;

public:
  BoundsInfos(
    Function&,
    const DataLayout&,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  );

  /// Mark the given pointer as having the given bounds information.
  void mark_as(const Value* ptr, const BoundsInfo binfo) {
    map[ptr] = binfo;
  }

  /// Get the bounds information for the given pointer.
  BoundsInfo get_binfo(const Value* ptr) const;

  /// Is there any bounds information for the given pointer?
  bool is_binfo_present(const Value* ptr) {
    return get_binfo(ptr).get_kind() != BoundsInfo::NOTDEFINEDYET;
  }

  void propagate_bounds(StoreInst&);
  void propagate_bounds(AllocaInst&);
  void propagate_bounds(GetElementPtrInst&, const DataLayout&);
  void propagate_bounds(SelectInst&);
  void propagate_bounds(IntToPtrInst&, PointerKind inttoptr_kind);
  void propagate_bounds(PHINode&);
  void propagate_bounds(CallBase& call, IsAllocatingCall& IAC);

  /// Copy the bounds for the input pointer (must be operand 0) to the output
  /// pointer
  void propagate_bounds_id(Instruction& inst);

  size_t numTrackedPtrs() const {
    return map.size();
  }

private:
  const DataLayout& DL;

  /// Reference to the `added_insts` where we note any instructions added for
  /// bounds purposes. See notes on `added_insts` in `DMSAnalysis`
  DenseSet<const Instruction*>& added_insts;

  /// Reference to the `pointer_aliases` for this function; see notes on
  /// `pointer_aliases` in `DMSAnalysis`
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases;

  /// Like `get_binfo()`, but doesn't check aliases of the given ptr, if any
  /// exist. This is used internally by `get_binfo()`.
  BoundsInfo get_binfo_noalias(const Value* ptr) const;

  BoundsInfo bounds_info_for_gep(GetElementPtrInst& gep, const DataLayout& DL) const;

  /// Value type for the below map
  class BoundsStoringCall final {
  public:
    /// Call instruction responsible for storing the bounds info
    Instruction* call_inst;
    /// Copy of the BoundsInfo which that Call instruction is storing
    BoundsInfo binfo;

    BoundsStoringCall() : call_inst(NULL), binfo(BoundsInfo()) {}
    BoundsStoringCall(Instruction* call_inst, BoundsInfo binfo) : call_inst(call_inst), binfo(binfo) {}

    // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
    // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
    friend void swap(BoundsStoringCall& A, BoundsStoringCall& B) noexcept {
      std::swap(A.call_inst, B.call_inst);
      BoundsInfo::swap(A.binfo, B.binfo);
    }
    BoundsStoringCall(BoundsStoringCall&& other) noexcept : BoundsStoringCall() {
      swap(*this, other);
    }
    BoundsStoringCall& operator=(BoundsStoringCall rhs) noexcept {
      swap(*this, rhs);
      return *this;
    }
  };

  /// This maps Store instructions (which store pointers) to `BoundsStoringCall`s.
  /// The intent is that on a subsequent iteration, if the BoundsInfo for the
  /// stored pointer has changed, we can remove the Call instruction and
  /// generate a new one.
  DenseMap<const StoreInst*, BoundsStoringCall> store_bounds_calls;
};

} // end namespace
