#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
#include "llvm/Transforms/Utils/DMS_RuntimeStackSlots.h"

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
  // A `BoundsInfo` can take one of these forms:

  /// The pointer has not been defined yet at this program point (at least, to
  /// our current knowledge). All pointers are (effectively) initialized to
  /// NotDefinedYet at the beginning of the analysis. Whenever it encounters a
  /// pointer definition, the DMS pass should also note bounds for the pointer
  /// -- at a bare minimum, marking the bounds Unknown instead of NotDefinedYet.
  class NotDefinedYet {
    // no additional information needed
    public:
    NotDefinedYet() = default;
    bool operator==(const NotDefinedYet& other) const { return true; }
    bool operator!=(const NotDefinedYet& other) const { return !(*this == other); }
  };

  /// Bounds info is not known for this pointer. In the future, dereferencing
  /// these pointers should be a compile-time error - we should know bounds
  /// info for all pointers which are ever dereferenced.
  class Unknown {
    // no additional information needed
    public:
    Unknown() = default;
    bool operator==(const Unknown& other) const { return true; }
    bool operator!=(const Unknown& other) const { return !(*this == other); }
  };

  /// This pointer should be considered to have infinite bounds in both
  /// directions
  class Infinite {
    // no additional information needed
    public:
    Infinite() = default;
    bool operator==(const Infinite& other) const { return true; }
    bool operator!=(const Infinite& other) const { return !(*this == other); }
  };

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
  class Static final {
  public:
    APInt low_offset;
    APInt high_offset;

    explicit Static(APInt low_offset, APInt high_offset)
      : low_offset(low_offset), high_offset(high_offset) {}
    explicit Static(uint64_t low_offset, uint64_t high_offset)
      : low_offset(APInt(/* numBits = */ 64, /* val = */ low_offset)),
        high_offset(APInt(/* numBits = */ 64, /* val = */ high_offset)) {}
    Static() : low_offset(zero), high_offset(minusone) {}

    bool operator==(const Static& other) const {
      return (low_offset == other.low_offset && high_offset == other.high_offset);
    }
    bool operator!=(const Static& other) const {
      return !(*this == other);
    }

    /// `cur_ptr`: the pointer value for which these static bounds apply.
    ///
    /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
    ///
    /// Returns the "base" (pointer to the first byte) of the allocation, as an
    /// LLVM `Value` of type `i8*`.
    Value* base_as_llvm_value(const Value* cur_ptr, DMSIRBuilder& Builder) const {
      return Builder.add_offset_to_ptr(cur_ptr, low_offset);
    }

    /// `cur_ptr`: the pointer value for which these static bounds apply.
    ///
    /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
    ///
    /// Returns the "max" (pointer to the last byte) of the allocation, as an
    /// LLVM `Value` of type `i8*`.
    Value* max_as_llvm_value(const Value* cur_ptr, DMSIRBuilder& Builder) const {
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

    std::string pretty() const;
  }; // end class Static

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
    PointerWithOffset(const PointerWithOffset&) = default;
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
  struct Dynamic {
    /// Gives the "base", i.e., pointer to the first byte of the allocation.
    /// Forces the bounds info to be computed if it hasn't been already.
    const PointerWithOffset& getBase() const {
      // we "cheat" the const here by calling the non-const `force()`
      (const_cast<Dynamic*>(this))->force();
      return std::get<Info>(data).base;
    }

    /// Gives the "max", i.e., pointer to the last byte of the allocation.
    /// Forces the bounds info to be computed if it hasn't been already.
    const PointerWithOffset& getMax() const {
      // we "cheat" the const here by calling the non-const `force()`
      (const_cast<Dynamic*>(this))->force();
      return std::get<Info>(data).max;
    }

    void force() {
      assert(!data.valueless_by_exception());
      if (LazyInfo* lazy = std::get_if<LazyInfo>(&data)) {
        data.emplace<Info>(lazy->force());
        assert(std::holds_alternative<Info>(data));
      } else if (LazyGlobalArraySize* lazy = std::get_if<LazyGlobalArraySize>(&data)) {
        data.emplace<Info>(lazy->force());
        assert(std::holds_alternative<Info>(data));
      }
      assert(std::holds_alternative<Info>(data));
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
      // copy and move are OK for Info
      Info(const Info&) = default;
      Info(Info&& other) noexcept : Info() {
        swap(*this, other);
      }
      // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
      // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
      /*friend*/ static void swap(Info& A, Info& B) noexcept {
        std::swap(A.base, B.base);
        std::swap(A.max, B.max);
      }
      Info& operator=(Info rhs) noexcept {
        swap(*this, rhs);
        return *this;
      }

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
      DenseSet<const Instruction*>* added_insts;
      /// Reference to the `runtime_stack_slots` for this function. See notes
      /// on `RuntimeStackSlots`
      RuntimeStackSlots* runtime_stack_slots;

      explicit LazyInfo(
        Value* addr,
        Instruction* loaded_ptr,
        DenseSet<const Instruction*>& added_insts,
        RuntimeStackSlots& runtime_stack_slots
      ) : addr(addr), loaded_ptr(loaded_ptr), added_insts(&added_insts), runtime_stack_slots(&runtime_stack_slots) {}
      LazyInfo() : addr(NULL), loaded_ptr(NULL), added_insts(NULL), runtime_stack_slots(NULL) {}

      // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
      // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
      /*friend*/ static void swap(LazyInfo& A, LazyInfo& B) noexcept {
        std::swap(A.addr, B.addr);
        std::swap(A.loaded_ptr, B.loaded_ptr);
        std::swap(A.added_insts, B.added_insts);
        std::swap(A.runtime_stack_slots, B.runtime_stack_slots);
      }
      LazyInfo& operator=(LazyInfo rhs) noexcept {
        swap(*this, rhs);
        return *this;
      }

      /// disallow copies: we only want to force() once
      LazyInfo(const LazyInfo&) = delete;
      /// move is OK though
      LazyInfo(LazyInfo&& other) noexcept : LazyInfo() {
        swap(*this, other);
      }

      Info force();

      bool operator==(const LazyInfo& other) const {
        // don't need to check that added_insts or runtime_stack_slots are equal
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
      /// Global array in question. Cannot be NULL
      GlobalValue* arr;
      /// `Function` which we should insert dynamic instructions in, in the
      /// event that we need to call `force()` to actually do the dynamic
      /// lookup. Cannot be NULL.
      Function* insertion_func;
      /// Reference to the `added_insts` where we note any instructions added
      /// for bounds purposes. See notes on `added_insts` in `DMSAnalysis`
      DenseSet<const Instruction*>* added_insts;
      /// Reference to the `runtime_stack_slots` for this function. See notes
      /// on `RuntimeStackSlots`
      RuntimeStackSlots* runtime_stack_slots;

      explicit LazyGlobalArraySize(
        GlobalValue& arr,
        Function& insertion_func,
        DenseSet<const Instruction*>& added_insts,
        RuntimeStackSlots& runtime_stack_slots
      ) : arr(&arr), insertion_func(&insertion_func), added_insts(&added_insts), runtime_stack_slots(&runtime_stack_slots) {}
      LazyGlobalArraySize() : arr(NULL), insertion_func(NULL), added_insts(NULL), runtime_stack_slots(NULL) {}

      // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
      // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
      /*friend*/ static void swap(LazyGlobalArraySize& A, LazyGlobalArraySize& B) noexcept {
        std::swap(A.arr, B.arr);
        std::swap(A.insertion_func, B.insertion_func);
        std::swap(A.added_insts, B.added_insts);
        std::swap(A.runtime_stack_slots, B.runtime_stack_slots);
      }
      LazyGlobalArraySize& operator=(LazyGlobalArraySize rhs) noexcept {
        swap(*this, rhs);
        return *this;
      }

      /// disallow copies: we only want to force() once
      LazyGlobalArraySize(const LazyGlobalArraySize&) = delete;
      /// move is OK though
      LazyGlobalArraySize(LazyGlobalArraySize&& other) : LazyGlobalArraySize() {
        swap(*this, other);
      }

      Info force();

      bool operator==(const LazyGlobalArraySize& other) const {
        // don't need to check that insertion_func, added_insts, or runtime_stack_slots are equal
        return (&arr == &other.arr);
      }
      bool operator!=(const LazyGlobalArraySize& other) const {
        return !(*this == other);
      }
    };

    /// Holds the actual data: either a real `Info`, or one of two ways to
    /// lazily compute it when it is needed
    std::variant<Info, LazyInfo, LazyGlobalArraySize> data;

  public:
    explicit Dynamic(Value* base, Value* max)
      : data(Info(base, max)) {}
    explicit Dynamic(PointerWithOffset base, PointerWithOffset max)
      : data(Info(base, max)) {}
    Dynamic(LazyInfo&& lazy) : data(std::move(lazy)) {}
    Dynamic(LazyGlobalArraySize&& lazy) : data(std::move(lazy)) {}
    Dynamic() : data(Info()) {}

    // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
    // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
    /*friend*/ static void swap(Dynamic& A, Dynamic& B) noexcept {
      std::swap(A.data, B.data);
    }
    Dynamic(Dynamic&& other) noexcept : Dynamic() {
      swap(*this, other);
    }
    Dynamic& operator=(Dynamic rhs) noexcept {
      swap(*this, rhs);
      return *this;
    }

    /// Disallow copies of Dynamic. Try to just use another pointer to the same
    /// Dynamic instead. This way, if the Dynamic is force()d by any user, all
    /// the pointers referencing it see the new Info without having to force()
    /// multiple times
    Dynamic(const Dynamic&) = delete;

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
    ///
    /// `runtime_stack_slots`: Reference to the `runtime_stack_slots` for this
    /// function. See notes on `RuntimeStackSlots`
    static Dynamic LazyDynamic(
      Value* addr,
      Instruction* loaded_ptr,
      DenseSet<const Instruction*>& added_insts,
      RuntimeStackSlots& runtime_stack_slots
    ) {
      return Dynamic(LazyInfo(addr, loaded_ptr, added_insts, runtime_stack_slots));
    }

    /// This is used to dynamically lookup the size of a global array. We only
    /// do this if the global array size is 0 in this compilation unit, eg
    /// because of a declaration like `extern int some_arr[];`.
    static Dynamic LazyDynamicGlobalArrayBounds(
      GlobalValue& arr,
      Function& insertion_func,
      DenseSet<const Instruction*>& added_insts,
      RuntimeStackSlots& runtime_stack_slots
    ) {
      return Dynamic(LazyGlobalArraySize(arr, insertion_func, added_insts, runtime_stack_slots));
    }

    // operator== and operator!= intentionally ignore the .merge_inputs field
    // here
    bool operator==(const Dynamic& other) const {
      return (data == other.data);
    }
    bool operator!=(const Dynamic& other) const {
      return !(*this == other);
    }

    /// A and B need not have any particular lifetime here
    static Dynamic merge(Dynamic& A, Dynamic& B, DMSIRBuilder& Builder);

    /// Create a Dynamic from the given Static and the pointer which the bounds
    /// apply to
    static Dynamic from_static(Static s, const Value* cur_ptr, DMSIRBuilder& Builder);
  }; // end class Dynamic

  /// Actual data held in the `BoundsInfo`: one of the above forms
  std::variant<NotDefinedYet, Unknown, Infinite, Static, Dynamic> data;

  bool valid() const {
    return !data.valueless_by_exception();
  }

  bool is_notdefinedyet() const {
    assert(valid());
    return std::holds_alternative<NotDefinedYet>(data);
  }

  bool is_unknown() const {
    assert(valid());
    return std::holds_alternative<Unknown>(data);
  }

  bool is_infinite() const {
    assert(valid());
    return std::holds_alternative<Infinite>(data);
  }

  bool is_static() const {
    assert(valid());
    return std::holds_alternative<Static>(data);
  }

  bool is_dynamic() const {
    assert(valid());
    return std::holds_alternative<Dynamic>(data);
  }

  const char* pretty_kind() const {
    switch (data.index()) {
      case 0: return "NotDefinedYet";
      case 1: return "Unknown";
      case 2: return "Infinite";
      case 3: return "Static";
      case 4: return "Dynamic";
      case std::variant_npos: llvm_unreachable("Invalid BoundsInfo"); // see docs on valueless_by_exception
      default: llvm_unreachable("Missing case in pretty_kind()");
    }
  }

  std::string pretty() const;

  /// Construct a BoundsInfo from a NotDefinedYet
  BoundsInfo(NotDefinedYet data) : data(data) {}

  /// Construct a BoundsInfo for a not-defined-yet pointer
  static BoundsInfo notdefinedyet() {
    return NotDefinedYet();
  }

  /// Construct a BoundsInfo from an Unknown
  BoundsInfo(Unknown data) : data(data) {}

  /// Construct a BoundsInfo with unknown bounds
  static BoundsInfo unknown() {
    return Unknown();
  }

  /// Construct a BoundsInfo from an Infinite
  BoundsInfo(Infinite data) : data(data) {}

  /// Construct a BoundsInfo with infinite bounds in both directions
  static BoundsInfo infinite() {
    return Infinite();
  }

  /// Construct a BoundsInfo with the given `Static`
  BoundsInfo(Static static_info) : data(static_info) {}

  /// Construct a BoundsInfo with the given static `low_offset` and
  /// `high_offset`
  static BoundsInfo static_bounds(APInt low_offset, APInt high_offset) {
    return BoundsInfo(Static(low_offset, high_offset));
  }
  /// Construct a BoundsInfo with the given static `low_offset` and
  /// `high_offset`
  static BoundsInfo static_bounds(uint64_t low_offset, uint64_t high_offset) {
    return BoundsInfo(Static(low_offset, high_offset));
  }

  /// Construct a BoundsInfo with the given `Dynamic`
  BoundsInfo(Dynamic&& dynamic_info) : data(std::move(dynamic_info)) {}

  /// Construct a BoundsInfo with the given dynamic `base` and `max`
  static BoundsInfo dynamic_bounds(Value* base, Value* max) {
    return BoundsInfo(Dynamic(base, max));
  }

  /// Construct a BoundsInfo with the given dynamic `base` and `max`
  static BoundsInfo dynamic_bounds(PointerWithOffset base, PointerWithOffset max) {
    return BoundsInfo(Dynamic(base, max));
  }

  /// The default constructor produces a NotDefinedYet
  explicit BoundsInfo() : data(NotDefinedYet()) {}

  // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
  // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
  /*friend*/ static void swap(BoundsInfo& A, BoundsInfo& B) noexcept {
    std::swap(A.data, B.data);
  }
  BoundsInfo(BoundsInfo&& other) noexcept : BoundsInfo() {
    swap(*this, other);
  }
  BoundsInfo& operator=(BoundsInfo rhs) noexcept {
    swap(*this, rhs);
    return *this;
  }

  /// Disallow copies of BoundsInfo; try to use another pointer to the same
  /// BoundsInfo instead. See notes on BoundsInfo::Dynamic
  BoundsInfo(const BoundsInfo&) = delete;

  bool operator==(const BoundsInfo& other) const {
    return (data == other.data);
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
  /// Or, if the BoundsInfo is Unknown or Infinite, returns NULL.
  /// The BoundsInfo should not be NotDefinedYet.
  Value* base_as_llvm_value(const Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// `cur_ptr`: the pointer value for which these bounds apply.
  ///
  /// `Builder`: the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the "max" (pointer to the last byte) of the allocation,
  /// as an LLVM `Value` of type `i8*`.
  /// Or, if the BoundsInfo is Unknown or Infinite, returns NULL.
  /// The BoundsInfo should not be NotDefinedYet.
  Value* max_as_llvm_value(const Value* cur_ptr, DMSIRBuilder& Builder) const;

  /// Insert dynamic instructions to store this bounds info.
  ///
  /// `ptr` is the pointer this bounds info applies to; call it P. `addr` is &P.
  /// Both `ptr` and `addr` need to be _decoded_ pointer values.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions.
  ///
  /// Returns the Call instruction if one was inserted, or else NULL
  CallInst* store_dynamic(Value* addr, const Value* ptr, DMSIRBuilder& Builder) const;

  /// Get a `Dynamic` representing dynamic bounds for the pointer
  /// `loaded_ptr`, which should have been loaded from the given `addr`. (I.e.,
  /// `addr == &loaded_ptr`.)
  /// (Both `addr` and `loaded_ptr` should be _decoded_ pointer values.)
  ///
  /// Computes the actual bounds lazily, i.e., does not insert any dynamic
  /// instructions unless/until this `Dynamic` is actually needed for a bounds
  /// check.
  ///
  /// Bounds info for this `addr` should have been previously stored with
  /// `store_dynamic()`.
  static Dynamic dynamic_bounds_for_ptr(
    Value* addr,
    Instruction* loaded_ptr,
    DenseSet<const Instruction*>& added_insts,
    RuntimeStackSlots& runtime_stack_slots
  ) {
    return Dynamic::LazyDynamic(addr, loaded_ptr, added_insts, runtime_stack_slots);
  }

  /// Get a `Dynamic` representing dynamic bounds for the global array
  /// `arr`. If dynamic instructions need to be inserted, insert them at the top
  /// of `insertion_func`.
  ///
  /// Computes the actual bounds lazily, i.e., does not insert any dynamic
  /// instructions unless/until this `Dynamic` is actually needed for
  /// a bounds check.
  ///
  /// This is only used for global arrays which have size 0 in this compilation
  /// unit (eg because of a declaration like `extern int some_arr[];`), in which
  /// case we need a dynamic lookup to find the actual bounds. For all other
  /// global arrays we know the bounds statically.
  static Dynamic dynamic_bounds_for_global_array(
    GlobalValue& arr,
    Function& insertion_func,
    DenseSet<const Instruction*>& added_insts,
    RuntimeStackSlots& runtime_stack_slots
  ) {
    return Dynamic::LazyDynamicGlobalArrayBounds(arr, insertion_func, added_insts, runtime_stack_slots);
  }
}; // end BoundsInfo

} // end namespace
