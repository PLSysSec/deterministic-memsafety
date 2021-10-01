#ifndef DMS_IRBUILDER_H
#define DMS_IRBUILDER_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/IRBuilder.h"

namespace llvm {

class DMSIRBuilder final : public IRBuilder<ConstantFolder, IRBuilderCallbackInserter> {
private:
	typedef IRBuilderCallbackInserter Inserter;
	typedef DenseSet<const Instruction*> InstSet;
	InstSet* inserted_insts; // if NULL, don't track inserted insts

public:
	enum BeforeOrAfter {
		/// Insert new instructions _before_ the given instruction
		BEFORE,
		/// Insert new instructions _after_ the given instruction
		AFTER
	};
	enum BeginningOrEnd {
		/// Insert new instructions at the _beginning_ of the given block (after the
		/// PHIs, if any)
		BEGINNING,
		/// Insert new instructions at the _end_ of the given block (before its
		/// terminator)
		END,
	};

	explicit DMSIRBuilder(BasicBlock* bb, BeginningOrEnd pos, InstSet* inserted_insts)
		: IRBuilder(bb->getContext(), ConstantFolder(), Inserter([this](const Instruction* inst) mutable { this->insert(inst); })),
			inserted_insts(inserted_insts)
		{
			if (pos == BEGINNING) SetInsertPoint(bb, bb->getFirstInsertionPt());
			else if (pos == END) SetInsertPoint(bb->getTerminator());
			else llvm_unreachable("Unhandled case");
		}
	explicit DMSIRBuilder(Instruction* inst, BeforeOrAfter pos, InstSet* inserted_insts)
		: IRBuilder(inst->getContext(), ConstantFolder(), Inserter([this](const Instruction* inst) mutable { this->insert(inst); })),
			inserted_insts(inserted_insts)
		{
			if (pos == BEFORE) SetInsertPoint(inst);
			else if (pos == AFTER) SetInsertPointToAfterInst(inst);
			else llvm_unreachable("Unhandled case");
		}
	explicit DMSIRBuilder(BasicBlock* bb, BasicBlock::iterator I, InstSet* inserted_insts)
		: IRBuilder(bb->getContext(), ConstantFolder(), Inserter([this](const Instruction* inst) mutable { this->insert(inst); })),
			inserted_insts(inserted_insts)
	{
		SetInsertPoint(bb, I);
	}

	/// Casts the given input pointer `ptr` to the LLVM type `i8*`.
	/// The input pointer can be any pointer type, including `i8*` (in which case
	/// this will return the pointer unchanged).
	Value* castToCharStar(Value* ptr);

	/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
	/// the resulting pointer.
	/// The input pointer can be any pointer type, the output pointer will
	/// have type `i8*`.
	Value* add_offset_to_ptr(Value* ptr, const APInt offset);

	/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
	/// the resulting pointer.
	/// The input pointer can be any pointer type, the output pointer will
	/// have type `i8*`.
	/// `offset` should be a non-pointer value -- ie, the number of bytes.
	Value* add_offset_to_ptr(Value* ptr, Value* offset);

private:
	/// Make this `Builder` ready to insert instructions _after_ the given `inst`
	void SetInsertPointToAfterInst(Instruction* inst);

	/// Insert the given instruction into `inserted_insts`
	void insert(const Instruction* inst) {
		if (inserted_insts) inserted_insts->insert(inst);
	}
};

} // end namespace

#endif
