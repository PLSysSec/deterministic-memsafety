#pragma once

#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"

namespace llvm {

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

  /// Call this (at least) once for each source _module_ (not function)
  static void module_initialization(
    Module&,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  );

  /// Mark the given pointer as having the given bounds information.
  void mark_as(const Value* ptr, const BoundsInfo binfo) {
    map[ptr] = binfo;
  }

  /// Get the bounds information for the given pointer.
  const BoundsInfo& get_binfo(const Value* ptr);

  /// Is there any bounds information for the given pointer?
  bool is_binfo_present(const Value* ptr) {
    return !get_binfo(ptr).is_notdefinedyet();
  }

  void propagate_bounds(StoreInst&, Value* override_stored_ptr);
  void propagate_bounds(AllocaInst&);
  void propagate_bounds(GetElementPtrInst&);
  void propagate_bounds(SelectInst&);
  void propagate_bounds(IntToPtrInst&, PointerKind inttoptr_kind);
  void propagate_bounds(LoadInst& load, Instruction* loaded_ptr); // the loaded_ptr may be different from the literal result of the `load` due to pointer encoding
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

  /// a BoundsInfo::infinite() which we can return a reference to
  const BoundsInfo infinite_binfo;
  /// a BoundsInfo::notdefinedyet() which we can return a reference to
  const BoundsInfo notdefinedyet_binfo;

  /// For all pointer expressions used in the given `Constant` (which we assume
  /// is the initializer for the given `addr`), make entries in the dynamic
  /// bounds table for each pointer expression. (This includes, eg, pointers to
  /// global variables, GEPs of such pointers, etc.)
  ///
  /// If dynamic instructions need to be inserted, use `Builder`.
  void store_info_for_all_ptr_exprs(Value* addr, Constant*, DMSIRBuilder&);

  /// Like `get_binfo()`, but doesn't check aliases of the given ptr, if any
  /// exist. This is used internally by `get_binfo()`.
  const BoundsInfo& get_binfo_noalias(const Value* ptr);

  const BoundsInfo& bounds_info_for_gep(GetElementPtrInst& gep);

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
