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
	return CreatePointerCast(ptr, getInt8PtrTy());
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
