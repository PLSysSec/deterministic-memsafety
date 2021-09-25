#include "llvm/Transforms/Utils/DMS_DynamicResults.h"

#include "llvm/ADT/Twine.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Regex.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

extern const APInt zero;

static Constant* createGlobalConstStr(Module& mod, const char* global_name, const char* str) {
  LLVMContext& ctx = mod.getContext();
  Constant* strConst = ConstantDataArray::getString(ctx, str);
  Constant* strGlobal = mod.getOrInsertGlobal(global_name, strConst->getType());
  cast<GlobalVariable>(strGlobal)->setInitializer(strConst);
  cast<GlobalVariable>(strGlobal)->setLinkage(GlobalValue::PrivateLinkage);
  return strGlobal;
}

static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String) {
  std::string curString = String.str();
  while (R.match(curString)) {
    curString = R.sub(Repl, curString);
  }
  return curString;
}

static Constant* findOrCreateGlobalCounter(Module& mod, Twine Name) {
	// https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
	LLVMContext& ctx = mod.getContext();
	Type* i64ty = IntegerType::getInt64Ty(ctx);

	Constant* global = mod.getOrInsertGlobal(Name.str(), i64ty);
	GlobalVariable* gv = cast<GlobalVariable>(global);
	if (!gv->hasInitializer()) {
		gv->setLinkage(GlobalValue::PrivateLinkage);
		gv->setAlignment(MaybeAlign(8));
		gv->setInitializer(ConstantInt::get(ctx, zero));
	}

	return global;
}

DynamicCounts::DynamicCounts(Module& mod, StringRef thingToCount) :
	clean(findOrCreateGlobalCounter(mod, thingToCount + "_clean")),
	blemished16(findOrCreateGlobalCounter(mod, thingToCount + "_blemished16")),
	blemished32(findOrCreateGlobalCounter(mod, thingToCount + "_blemished32")),
	blemished64(findOrCreateGlobalCounter(mod, thingToCount + "_blemished64")),
	blemishedconst(findOrCreateGlobalCounter(mod, thingToCount + "_blemishedconst")),
	dirty(findOrCreateGlobalCounter(mod, thingToCount + "_dirty")),
	unknown(findOrCreateGlobalCounter(mod, thingToCount + "_unknown"))
{}

DynamicResults::DynamicResults(Module& mod) :
	load_addrs(DynamicCounts(mod, "__DMS_load_addrs")),
	store_addrs(DynamicCounts(mod, "__DMS_store_addrs")),
	store_vals(DynamicCounts(mod, "__DMS_store_vals")),
	passed_ptrs(DynamicCounts(mod, "__DMS_passed_ptrs")),
	returned_ptrs(DynamicCounts(mod, "__DMS_returned_ptrs")),
	pointer_arith_const(DynamicCounts(mod, "__DMS_pointer_arith_const")),
	inttoptrs(findOrCreateGlobalCounter(mod, "__DMS_inttoptrs")),
	mod(mod)
{}

void DynamicResults::addPrint(DynamicResults::PrintType print_type) {
	// https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
	LLVMContext& ctx = mod.getContext();

	// if this function already exists in the module, assume we've already added
	// the print
	if (mod.getFunction("__DMS_output_wrapper")) {
		return;
	}

	std::string output = "";
	output += "================\n";
	output += "DMS dynamic counts for " + mod.getName().str() + ":\n";
	output += "================\n";
	output += "Loads with clean addr: %llu\n";
	output += "Loads with blemished16 addr: %llu\n";
	output += "Loads with blemished32 addr: %llu\n";
	output += "Loads with blemished64 addr: %llu\n";
	output += "Loads with blemishedconst addr: %llu\n";
	output += "Loads with dirty addr: %llu\n";
	output += "Loads with unknown addr: %llu\n";
	output += "Stores with clean addr: %llu\n";
	output += "Stores with blemished16 addr: %llu\n";
	output += "Stores with blemished32 addr: %llu\n";
	output += "Stores with blemished64 addr: %llu\n";
	output += "Stores with blemishedconst addr: %llu\n";
	output += "Stores with dirty addr: %llu\n";
	output += "Stores with unknown addr: %llu\n";
	output += "Storing a clean ptr to mem: %llu\n";
	output += "Storing a blemished16 ptr to mem: %llu\n";
	output += "Storing a blemished32 ptr to mem: %llu\n";
	output += "Storing a blemished64 ptr to mem: %llu\n";
	output += "Storing a blemishedconst ptr to mem: %llu\n";
	output += "Storing a dirty ptr to mem: %llu\n";
	output += "Storing an unknown ptr to mem: %llu\n";
	output += "Passing a clean ptr to a func: %llu\n";
	output += "Passing a blemished16 ptr to a func: %llu\n";
	output += "Passing a blemished32 ptr to a func: %llu\n";
	output += "Passing a blemished64 ptr to a func: %llu\n";
	output += "Passing a blemishedconst ptr to a func: %llu\n";
	output += "Passing a dirty ptr to a func: %llu\n";
	output += "Passing an unknown ptr to a func: %llu\n";
	output += "Returning a clean ptr from a func: %llu\n";
	output += "Returning a blemished16 ptr from a func: %llu\n";
	output += "Returning a blemished32 ptr from a func: %llu\n";
	output += "Returning a blemished64 ptr from a func: %llu\n";
	output += "Returning a blemishedconst ptr from a func: %llu\n";
	output += "Returning a dirty ptr from a func: %llu\n";
	output += "Returning an unknown ptr from a func: %llu\n";
	output += "Nonzero constant pointer arithmetic on a clean ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on a blemished16 ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on a blemished32 ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on a blemished64 ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on a blemishedconst ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on a dirty ptr: %llu\n";
	output += "Nonzero constant pointer arithmetic on an unknown ptr: %llu\n";
	output += "Producing a ptr from inttoptr: %llu\n";
	output += "\n";

	// Inject a global variable to hold the output string
	Constant* OutputStr = createGlobalConstStr(mod, "__DMS_output_str", output.c_str());

	// Create a void function which calls printf() or fprintf() to print the
	// output
	Type* i8ty = IntegerType::getInt8Ty(ctx);
	Type* i8StarTy = PointerType::getUnqual(i8ty);
	Type* i32ty = IntegerType::getInt32Ty(ctx);
	Type* i32StarTy = PointerType::getUnqual(i32ty);
	Type* i64ty = IntegerType::getInt64Ty(ctx);
	FunctionType* WrapperTy = FunctionType::get(Type::getVoidTy(ctx), {}, false);
	Function* Wrapper_func = cast<Function>(mod.getOrInsertFunction("__DMS_output_wrapper", WrapperTy).getCallee());
	Wrapper_func->setLinkage(GlobalValue::PrivateLinkage);
	BasicBlock* EntryBlock = BasicBlock::Create(ctx, "entry", Wrapper_func);
	IRBuilder<> Builder(EntryBlock);

	if (print_type == STDOUT) {
		// call printf()
		FunctionType* PrintfTy = FunctionType::get(i32ty, i8StarTy, /* IsVarArgs = */ true);
		FunctionCallee Printf = mod.getOrInsertFunction("printf", PrintfTy);
		//Function* Printf_func = cast<Function>(Printf.getCallee());
		//Printf_func->setDoesNotThrow();
		//Printf_func->addParamAttr(0, Attribute::NoCapture);
		//Printf_func->addParamAttr(0, Attribute::ReadOnly);
		Builder.CreateCall(Printf, {
			Builder.CreatePointerCast(OutputStr, i8StarTy),
			Builder.CreateLoad(i64ty, load_addrs.clean),
			Builder.CreateLoad(i64ty, load_addrs.blemished16),
			Builder.CreateLoad(i64ty, load_addrs.blemished32),
			Builder.CreateLoad(i64ty, load_addrs.blemished64),
			Builder.CreateLoad(i64ty, load_addrs.blemishedconst),
			Builder.CreateLoad(i64ty, load_addrs.dirty),
			Builder.CreateLoad(i64ty, load_addrs.unknown),
			Builder.CreateLoad(i64ty, store_addrs.clean),
			Builder.CreateLoad(i64ty, store_addrs.blemished16),
			Builder.CreateLoad(i64ty, store_addrs.blemished32),
			Builder.CreateLoad(i64ty, store_addrs.blemished64),
			Builder.CreateLoad(i64ty, store_addrs.blemishedconst),
			Builder.CreateLoad(i64ty, store_addrs.dirty),
			Builder.CreateLoad(i64ty, store_addrs.unknown),
			Builder.CreateLoad(i64ty, store_vals.clean),
			Builder.CreateLoad(i64ty, store_vals.blemished16),
			Builder.CreateLoad(i64ty, store_vals.blemished32),
			Builder.CreateLoad(i64ty, store_vals.blemished64),
			Builder.CreateLoad(i64ty, store_vals.blemishedconst),
			Builder.CreateLoad(i64ty, store_vals.dirty),
			Builder.CreateLoad(i64ty, store_vals.unknown),
			Builder.CreateLoad(i64ty, passed_ptrs.clean),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished16),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished32),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished64),
			Builder.CreateLoad(i64ty, passed_ptrs.blemishedconst),
			Builder.CreateLoad(i64ty, passed_ptrs.dirty),
			Builder.CreateLoad(i64ty, passed_ptrs.unknown),
			Builder.CreateLoad(i64ty, returned_ptrs.clean),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished16),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished32),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished64),
			Builder.CreateLoad(i64ty, returned_ptrs.blemishedconst),
			Builder.CreateLoad(i64ty, returned_ptrs.dirty),
			Builder.CreateLoad(i64ty, returned_ptrs.unknown),
			Builder.CreateLoad(i64ty, pointer_arith_const.clean),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished16),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished32),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished64),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemishedconst),
			Builder.CreateLoad(i64ty, pointer_arith_const.dirty),
			Builder.CreateLoad(i64ty, pointer_arith_const.unknown),
			Builder.CreateLoad(i64ty, inttoptrs),
		});
		Builder.CreateRetVoid();
	} else if (print_type == TOFILE) {
		// create strings for arguments to mkdir, fopen, and perror
		auto modNameNoDotDot = regexSubAll(Regex("\\.\\./"), "", mod.getName());
		auto modNameWithDots = regexSubAll(Regex("/"), ".", modNameNoDotDot);
		auto file_str = "./dms_dynamic_counts/" + modNameWithDots;
		Constant* DirStr = createGlobalConstStr(mod, "__DMS_dir_str", "./dms_dynamic_counts");
		Constant* FileStr = createGlobalConstStr(mod, "__DMS_file_str", file_str.c_str());
		Constant* ModeStr = createGlobalConstStr(mod, "__DMS_mode_str", "a");
		Constant* PerrorStr = createGlobalConstStr(mod, "__DMS_perror_str", ("Failed to open " + file_str).c_str());
		// call mkdir
		FunctionType* MkdirTy = FunctionType::get(i32ty, {i8StarTy, i32ty}, /* IsVarArgs = */ false);
		FunctionCallee Mkdir = mod.getOrInsertFunction("mkdir", MkdirTy);
		//Function* Mkdir_func = cast<Function>(Mkdir.getCallee());
		//Mkdir_func->addParamAttr(0, Attribute::NoCapture);
		//Mkdir_func->addParamAttr(0, Attribute::ReadOnly);
		Value* Mkdir_ret = Builder.CreateCall(Mkdir, {
			Builder.CreatePointerCast(DirStr, i8StarTy),
			Builder.getInt32(/* octal */ 0777),
		});
		// check for error from mkdir
		Value* cond = Builder.CreateICmpEQ(Mkdir_ret, Builder.getInt32(0));
		BasicBlock* ErrorBB = BasicBlock::Create(ctx, "error", Wrapper_func);
		BasicBlock* NoErrorBB = BasicBlock::Create(ctx, "noerror", Wrapper_func);
		Builder.CreateCondBr(cond, NoErrorBB, ErrorBB);
		// the case where mkdir returns error
		// see if errno is EEXIST (17), and if so, ignore the error.
		// otherwise return early and don't print anything.
		Builder.SetInsertPoint(ErrorBB);
		FunctionType* ErrnoTy = FunctionType::get(i32StarTy, {}, false);
		FunctionCallee Errno_callee = mod.getOrInsertFunction("__errno_location", ErrnoTy);
		//Function* Errno_func = cast<Function>(Errno_callee.getCallee());
		//Errno_func->setDoesNotAccessMemory();
		//Errno_func->setDoesNotThrow();
		//Errno_func->setWillReturn();
		Value* errno_addr = Builder.CreateCall(Errno_callee, {});
		Value* errno_val = Builder.CreateLoad(i32ty, errno_addr);
		cond = Builder.CreateICmpEQ(errno_val, Builder.getInt32(17));
		BasicBlock* JustReturnBB = BasicBlock::Create(ctx, "justreturn", Wrapper_func);
		Builder.CreateCondBr(cond, NoErrorBB, JustReturnBB);
		Builder.SetInsertPoint(JustReturnBB);
		Builder.CreateRetVoid();
		// the case where mkdir succeeds (or where we got EEXIST and ignored it -
		// in either case the directory now exists).
		// Call fopen
		Builder.SetInsertPoint(NoErrorBB);
		StructType* FileTy = StructType::create(ctx, "struct._IO_FILE");
		PointerType* FileStarTy = PointerType::getUnqual(FileTy);
		FunctionType* FopenTy = FunctionType::get(FileStarTy, {i8StarTy, i8StarTy}, false);
		FunctionCallee Fopen = mod.getOrInsertFunction("fopen", FopenTy);
		//Function* Fopen_func = cast<Function>(Fopen.getCallee());
		//Fopen_func->addParamAttr(0, Attribute::NoCapture);
		//Fopen_func->addParamAttr(0, Attribute::ReadOnly);
		//Fopen_func->addParamAttr(1, Attribute::NoCapture);
		//Fopen_func->addParamAttr(1, Attribute::ReadOnly);
		Value* file_handle = Builder.CreateCall(Fopen, {
			Builder.CreatePointerCast(FileStr, i8StarTy),
			Builder.CreatePointerCast(ModeStr, i8StarTy),
		});
		// check for error from fopen
		cond = Builder.CreateIsNull(file_handle);
		BasicBlock* WriteBB = BasicBlock::Create(ctx, "write", Wrapper_func);
		BasicBlock* FopenFailed = BasicBlock::Create(ctx, "fopenfailed", Wrapper_func);
		Builder.CreateCondBr(cond, FopenFailed, WriteBB);
		Builder.SetInsertPoint(FopenFailed);
		FunctionType* PerrorTy = FunctionType::get(Type::getVoidTy(ctx), {i8StarTy}, false);
		FunctionCallee Perror = mod.getOrInsertFunction("perror", PerrorTy);
		Builder.CreateCall(Perror, {Builder.CreatePointerCast(PerrorStr, i8StarTy)});
		Builder.CreateRetVoid();
		// and, actually write to file
		Builder.SetInsertPoint(WriteBB);
		FunctionType* FprintfTy = FunctionType::get(i32ty, {FileStarTy, i8StarTy}, /* IsVarArgs = */ true);
		FunctionCallee Fprintf = mod.getOrInsertFunction("fprintf", FprintfTy);
		Builder.CreateCall(Fprintf, {
			file_handle,
			Builder.CreatePointerCast(OutputStr, i8StarTy),
			Builder.CreateLoad(i64ty, load_addrs.clean),
			Builder.CreateLoad(i64ty, load_addrs.blemished16),
			Builder.CreateLoad(i64ty, load_addrs.blemished32),
			Builder.CreateLoad(i64ty, load_addrs.blemished64),
			Builder.CreateLoad(i64ty, load_addrs.blemishedconst),
			Builder.CreateLoad(i64ty, load_addrs.dirty),
			Builder.CreateLoad(i64ty, load_addrs.unknown),
			Builder.CreateLoad(i64ty, store_addrs.clean),
			Builder.CreateLoad(i64ty, store_addrs.blemished16),
			Builder.CreateLoad(i64ty, store_addrs.blemished32),
			Builder.CreateLoad(i64ty, store_addrs.blemished64),
			Builder.CreateLoad(i64ty, store_addrs.blemishedconst),
			Builder.CreateLoad(i64ty, store_addrs.dirty),
			Builder.CreateLoad(i64ty, store_addrs.unknown),
			Builder.CreateLoad(i64ty, store_vals.clean),
			Builder.CreateLoad(i64ty, store_vals.blemished16),
			Builder.CreateLoad(i64ty, store_vals.blemished32),
			Builder.CreateLoad(i64ty, store_vals.blemished64),
			Builder.CreateLoad(i64ty, store_vals.blemishedconst),
			Builder.CreateLoad(i64ty, store_vals.dirty),
			Builder.CreateLoad(i64ty, store_vals.unknown),
			Builder.CreateLoad(i64ty, passed_ptrs.clean),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished16),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished32),
			Builder.CreateLoad(i64ty, passed_ptrs.blemished64),
			Builder.CreateLoad(i64ty, passed_ptrs.blemishedconst),
			Builder.CreateLoad(i64ty, passed_ptrs.dirty),
			Builder.CreateLoad(i64ty, passed_ptrs.unknown),
			Builder.CreateLoad(i64ty, returned_ptrs.clean),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished16),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished32),
			Builder.CreateLoad(i64ty, returned_ptrs.blemished64),
			Builder.CreateLoad(i64ty, returned_ptrs.blemishedconst),
			Builder.CreateLoad(i64ty, returned_ptrs.dirty),
			Builder.CreateLoad(i64ty, returned_ptrs.unknown),
			Builder.CreateLoad(i64ty, pointer_arith_const.clean),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished16),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished32),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemished64),
			Builder.CreateLoad(i64ty, pointer_arith_const.blemishedconst),
			Builder.CreateLoad(i64ty, pointer_arith_const.dirty),
			Builder.CreateLoad(i64ty, pointer_arith_const.unknown),
			Builder.CreateLoad(i64ty, inttoptrs),
		});
		FunctionType* FcloseTy = FunctionType::get(i32ty, {FileStarTy}, false);
		FunctionCallee Fclose = mod.getOrInsertFunction("fclose", FcloseTy);
		Builder.CreateCall(Fclose, {file_handle});
		Builder.CreateBr(JustReturnBB);
	} else {
		llvm_unreachable("unexpected print_type\n");
	}

	// Inject this wrapper function into the GlobalDtors for the module
	appendToGlobalDtors(mod, Wrapper_func, /* Priority = */ 0);
}
