// Struct definitions and utility functions needed by multiple DMS files

#ifndef DMS_COMMON_H
#define DMS_COMMON_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"
#include <optional>

struct CallNameInfo {
  enum CallKind {
    /// Call of a function pointer, or etc
    INDIRECTCALL,
    /// Call of an anonymous function
    ANONCALL,
    /// Call of a function we know the name of
    NAMEDCALL,
  };

  /// Is the call indirect, anonymous, etc
  CallKind kind;
  /// If `kind` is `NAMEDCALL`, then this is the function name.
  /// Otherwise, this is undefined
  llvm::StringRef name;

  CallNameInfo(CallKind kind) : kind(kind) {}
  CallNameInfo(CallKind kind, llvm::StringRef name) : kind(kind), name(name) {}

  bool operator==(const CallNameInfo& other) const {
    if (kind != other.kind) return false;
    if (kind == NAMEDCALL && name != other.name) return false;
    return true;
  }
  bool operator!=(const CallNameInfo& other) const {
    return !(*this == other);
  }
};

CallNameInfo getCallNameInfo(const llvm::CallBase&);

/// Describes whether a call allocates memory, and if so, the size of
/// the memory allocated.
struct IsAllocatingCall {
  /// If this is an allocating call, this holds the allocation size in bytes.
  /// If this isn't an allocating call, this `optional` will be empty.
  std::optional<llvm::Value*> allocation_bytes;

  /// also in case you need it, here's the CallNameInfo
  CallNameInfo CNI;

  static IsAllocatingCall not_allocating(CallNameInfo CNI) { return IsAllocatingCall { std::nullopt, CNI }; }
  static IsAllocatingCall allocating(llvm::Value* size, CallNameInfo CNI) { return IsAllocatingCall { size, CNI }; }
};

/// If computing the allocation size requires inserting dynamic instructions,
/// use `Builder`
IsAllocatingCall isAllocatingCall(const llvm::CallBase&, llvm::DMSIRBuilder& Builder);

/// this struct would be a std::optional if we could use C++17
struct GEPConstantOffset {
  /// If the GEP's total offset is a compile-time constant, this holds the
  /// value of the constant offset, in bytes.
  /// Otherwise, this is empty.
  std::optional<llvm::APInt> constant_offset;

  std::string pretty() const {
    if (constant_offset.has_value()) {
      llvm::SmallVector<char> offset_str;
      constant_offset->toStringSigned(offset_str);
      return llvm::Twine("constant " + offset_str).str();
    }
    else return "nonconstant";
  }
};

/// Determine whether the GEP's total offset is a compile-time constant, and if
/// so, what constant
GEPConstantOffset computeGEPOffset(const llvm::GetElementPtrInst&, const llvm::DataLayout&);

/// Returns `true` if the block is well-formed. For this function's purposes,
/// "well-formed" means:
///   - the block has exactly one terminator instruction
///   - the terminator instruction is at the end
///   - all PHI instructions and/or landingpad instructions (if they exist) come first
bool wellFormed(const llvm::BasicBlock& bb);

/// This function iterates using the same pattern in `GlobalDCEPass`,
/// to ensure that no GV users are Instructions without a Block parent.
/// (If there were, `GlobalDCEPass` will crash later.)
void verifyGVUsersAreWellFormed(const llvm::Function& F);

/// Convenience function to create calls to our runtime support function
/// `__dms_store_bounds()`. See docs in dms_interface.h.
///
/// The arguments `addr`, `base`, and `max` can be any pointer type (not
/// necessarily `void*`). They should be UNENCODED values, ie with all upper
/// bits clear.
llvm::CallInst* call_dms_store_bounds(llvm::Value* addr, llvm::Value* base, llvm::Value* max, llvm::DMSIRBuilder& Builder);

/// Convenience function to create calls to our runtime support function
/// `__dms_store_infinite_bounds()`. See docs in dms_interface.h.
///
/// The `addr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
llvm::CallInst* call_dms_store_infinite_bounds(llvm::Value* addr, llvm::DMSIRBuilder& Builder);

/// Convenience function to create calls to our runtime support function
/// `__dms_get_bounds()`. See docs in dms_interface.h.
///
/// The `addr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
///
/// `output_base` and `output_max` are output parameters and should have LLVM
/// type i8**.
llvm::CallInst* call_dms_get_bounds(llvm::Value* addr, llvm::Value* output_base, llvm::Value* output_max, llvm::DMSIRBuilder& Builder);

/// Convenience function to create calls to our runtime support function
/// `__dms_store_globalarraysize()`. See docs in dms_interface.h.
///
/// The arguments `arr` and `max` can be any pointer type (not necessarily
/// `void*`). They should be UNENCODED values, ie with all upper bits clear.
llvm::CallInst* call_dms_store_globalarraysize(llvm::GlobalValue* arr, llvm::Value* max, llvm::DMSIRBuilder& Builder);

/// Convenience function to create calls to our runtime support function
/// `__dms_get_globalarraysize()`. See docs in dms_interface.h.
///
/// The `arr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
///
/// `output_max` is an output parameter and should have LLVM type i8**.
llvm::CallInst* call_dms_get_globalarraysize(llvm::GlobalValue* arr, llvm::Value* output_max, llvm::DMSIRBuilder& Builder);

/// Convenience function to create calls to our runtime support function
/// `__dms_boundscheckfail()`. See docs in dms_interface.h.
///
/// The `ptr` argument can be any pointer type (not necessarily `void*`),
/// and should be an UNENCODED value, ie with all upper bits clear.
llvm::CallInst* call_dms_boundscheckfail(llvm::Value* ptr, llvm::DMSIRBuilder& Builder);

#endif
