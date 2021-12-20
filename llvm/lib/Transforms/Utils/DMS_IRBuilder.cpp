#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"

using namespace llvm;

/// Make this `Builder` ready to insert instructions _after_ the given `inst`
void DMSIRBuilder::SetInsertPointToAfterInst(Instruction* inst) {
  SetInsertPoint(inst);
  BasicBlock* bb = GetInsertBlock();
  auto ip = GetInsertPoint();
  ip++;
  SetInsertPoint(bb, ip);
}

/// Casts the given input pointer `ptr` to the LLVM type `i8*`.
/// The input pointer can be any pointer type, including `i8*` (in which case
/// this will return the pointer unchanged).
Value* DMSIRBuilder::castToCharStar(Value* ptr) {
  assert(ptr->getType()->isPointerTy());
  std::string ptr_name = isa<ConstantExpr>(ptr) ? "constexpr" : ptr->getNameOrAsOperand();
  return CreatePointerCast(ptr, getInt8PtrTy(), Twine(ptr_name, "_casted"));
}

/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
/// the resulting pointer.
/// The input pointer can be any pointer type, the output pointer will
/// have type `i8*`.
Value* DMSIRBuilder::add_offset_to_ptr(Value* ptr, const APInt offset) {
  Value* casted = castToCharStar(ptr);
  if (offset == 0) {
    return casted;
  } else {
    return add_offset_to_ptr(casted, getInt(offset));
  }
}

/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
/// the resulting pointer.
/// The input pointer can be any pointer type, the output pointer will
/// have type `i8*`.
/// `offset` should be a non-pointer value -- ie, the number of bytes.
Value* DMSIRBuilder::add_offset_to_ptr(Value* ptr, Value* offset) {
  return CreateGEP(getInt8Ty(), castToCharStar(ptr), offset);
}

/// Let the Builder's current block be A.
/// Split A at the Builder's current insertion point. Everything before
/// the Builder's insertion point stays in A, while everything after it
/// is moved to a new block B.
/// At the end of A, insert a conditional jump based on `cond`.
/// If `cond` is true, execution jumps to `true_bb`; else, it continues
/// in B.
///
/// To be safe, assume that this `DMSIRBuilder` is invalidated by this function.
///
/// Returns the new block B, where execution continues if it doesn't branch
/// to `true_bb`.
BasicBlock* DMSIRBuilder::insertCondJumpTo(Value* cond, BasicBlock* true_bb) {
  BasicBlock* A = GetInsertBlock();
  BasicBlock::iterator I = GetInsertPoint();
  BasicBlock* B = A->splitBasicBlock(I);
  // To be safe, assume that `Builder` is invalidated by the above operation
  // (docs say that the iterator `I` is invalidated).

  // replace A's terminator with a condbr jumping to either
  // `true_bb` or B as appropriate
  BasicBlock::iterator A_end = A->getTerminator()->eraseFromParent();
  DMSIRBuilder NewBuilder(A, A_end, inserted_insts);
  NewBuilder.CreateCondBr(cond, true_bb, B);

  assert(wellFormed(*A));
  assert(wellFormed(*B));

  return B;
}
