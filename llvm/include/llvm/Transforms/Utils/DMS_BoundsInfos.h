#pragma once

#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"

#include "llvm/Support/Debug.h"

namespace llvm {

/// Holds the bounds information for all pointers in the function.
class BoundsInfos final {
private:
  /// This is a record of all heap-allocated BoundsInfo objects, so that
  /// we can free them all when this BoundsInfos is destructed.
  std::vector<BoundsInfo*> infos;
  /// a BoundsInfo::notdefinedyet() which we can return pointers to
  BoundsInfo notdefinedyet_binfo;
  /// a BoundsInfo::unknown() which we can return pointers to
  BoundsInfo unknown_binfo;
  /// a BoundsInfo::infinite() which we can return pointers to
  BoundsInfo infinite_binfo;

  /// Maps a pointer to its bounds info, if we know anything about its bounds
  /// info.
  /// For pointers not appearing in this map, we don't know anything about their
  /// bounds.
  ///
  /// The BoundsInfo* here should have the same lifetime as this `BoundsInfos`.
  /// Either it should be a heap pointer (a copy of which exists in `infos`, so
  /// that it will be freed in the `BoundsInfos` destructor), or it should be a
  /// pointer to one of the special BoundsInfo class members above.
  ///
  /// Many keys may map to the same BoundsInfo*.
  ///
  /// Neither keys nor values in this map should ever be NULL.
  DenseMap<const Value*, BoundsInfo*> map;

public:
  BoundsInfos(
    Function&,
    const DataLayout&,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  );

  ~BoundsInfos() {
    for (BoundsInfo* binfo : infos) {
      delete binfo;
    }
  }

  /// Call this (at least) once for each source _module_ (not function)
  static void module_initialization(
    Module&,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  );

  /// Mark the given pointer as having the given bounds information.
  ///
  /// The `binfo` pointer should have lifetime equal to the lifetime of this
  /// BoundsInfos -- eg, should be a heap pointer (which also exists in
  /// `infos`), or should be a pointer to one of the special BoundsInfo class
  /// members.
  void mark_as(const Value* ptr, BoundsInfo* binfo) {
    DEBUG_WITH_TYPE("DMS-bounds-info", dbgs() << "DMS:     bounds info for " << ptr->getNameOrAsOperand() << " marked as " << binfo->pretty() << "\n");
    map[ptr] = binfo;
  }

  /// Mark the given pointer as having the given bounds information.
  ///
  /// This version does heap allocation.
  /// If possible (ie if you already have a heap-allocated BoundsInfo*), prefer
  /// the version that passes a `BoundsInfo*`.
  void mark_as(const Value* ptr, BoundsInfo&& binfo) {
    BoundsInfo* heap_binfo = new BoundsInfo(std::move(binfo));
    infos.push_back(heap_binfo);
    mark_as(ptr, heap_binfo);
  }

  /// Mark the given pointer as having the merger of the two given bounds
  /// information.
  ///
  /// `A` and `B` should have lifetime equal to the lifetime of this BoundsInfos.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary.
  void mark_as_merged(
    Value* ptr,
    BoundsInfo& A,
    BoundsInfo& B,
    DMSIRBuilder& Builder
  );

  /// Get the bounds information for the given pointer.
  ///
  /// The returned pointer is never NULL, and should have lifetime equal
  /// to the lifetime of this BoundsInfos.
  BoundsInfo* get_binfo(const Value* ptr);

  /// Is there any bounds information for the given pointer?
  bool is_binfo_present(const Value* ptr) {
    return !get_binfo(ptr)->is_notdefinedyet();
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

  /// For all pointer expressions used in the given `Constant` (which we assume
  /// is the initializer for the given `addr`), make entries in the dynamic
  /// bounds table for each pointer expression. (This includes, eg, pointers to
  /// global variables, GEPs of such pointers, etc.)
  ///
  /// If dynamic instructions need to be inserted, use `Builder`.
  void store_info_for_all_ptr_exprs(Value* addr, Constant*, DMSIRBuilder&);

  /// Like `get_binfo()`, but doesn't check aliases of the given ptr, if any
  /// exist. This is used internally by `get_binfo()`.
  ///
  /// The returned pointer is never NULL, and should have lifetime equal
  /// to the lifetime of this BoundsInfos.
  BoundsInfo* get_binfo_noalias(const Value* ptr);

  /// Get bounds info for the given GEP.
  ///
  /// The returned pointer is never NULL, and should have lifetime equal
  /// to the lifetime of this BoundsInfos.
  BoundsInfo* bounds_info_for_gep(GetElementPtrInst& gep);

  /// Mark the given pointer as having the merger of the two given bounds
  /// information.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary.
  void mark_as_merged(
    Value* ptr,
    BoundsInfo::Static& static_info,
    BoundsInfo::Dynamic& dyn_info,
    DMSIRBuilder& Builder
  );

  /// Value type for the below map
  class BoundsStoringCall final {
  public:
    /// Call instruction responsible for storing the bounds info
    Instruction* call_inst;
    /// Copy of the BoundsInfo which that Call instruction is storing
    BoundsInfo* binfo;

    BoundsStoringCall() : call_inst(NULL), binfo(NULL) {}
    BoundsStoringCall(Instruction* call_inst, BoundsInfo* binfo) : call_inst(call_inst), binfo(binfo) {}

    // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
    // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
    friend void swap(BoundsStoringCall& A, BoundsStoringCall& B) noexcept {
      std::swap(A.call_inst, B.call_inst);
      std::swap(A.binfo, B.binfo);
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
