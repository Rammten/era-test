// This file is part of solidity.

// solidity is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// solidity is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with solidity.  If not, see <http://www.gnu.org/licenses/>.

// SPDX-License-Identifier: GPL-3.0

//
// Solidity dialect lowering pass
//

#include "libsolidity/codegen/mlir/Passes.h"
#include "libsolidity/codegen/mlir/Solidity/SolidityOps.h"
#include "libsolidity/codegen/mlir/Util.h"
#include "mlir/Conversion/ArithmeticToLLVM/ArithmeticToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>
#include <climits>
#include <vector>

using namespace mlir;

namespace {

// FIXME: The high level dialects are lowered to the llvm dialect tailored to
// the EraVM backend in llvm. How should we perform the lowering when we support
// other targets?
//
// (a) If we do a condition lowering in this pass, the code can quickly get
// messy
//
// (b) If we have a high level dialect for each target, the lowering will be,
// for instance, solidity.object -> eravm.object -> llvm.func with eravm
// details. Unnecessary abstractions?
//
// (c) I think a sensible design is to create different ModuleOp passes for each
// target that lower high level dialects to the llvm dialect.
//

namespace eravm {

//
// FIXME: Is it possible to define enum classes whose members can implicitly
// cast to unsigned? AddrSpace, {Byte, Bit}Len enums are mostly used as unsigned
// in the lowering (for instance, address space arguments in llvm are unsigned).
// For now, we're mimicking the scope with prefixes
//
enum AddrSpace : unsigned {
  AddrSpace_Stack = 0,
  AddrSpace_Heap = 1,
  AddrSpace_HeapAuxiliary = 2,
  AddrSpace_Generic = 3,
  AddrSpace_Code = 4,
  AddrSpace_Storage = 5,
};

enum ByteLen {
  ByteLen_Byte = 1,
  ByteLen_X32 = 4,
  ByteLen_X64 = 8,
  ByteLen_EthAddr = 20,
  ByteLen_Field = 32
};
enum BitLen {
  BitLen_Bool = 1,
  BitLen_Byte = 8,
  BitLen_X32 = BitLen_Byte * ByteLen_X32,
  BitLen_X64 = BitLen_Byte * ByteLen_X64,
  BitLen_EthAddr = BitLen_Byte * ByteLen_EthAddr,
  BitLen_Field = BitLen_Byte * ByteLen_Field
};

enum : unsigned { HeapAuxOffsetCtorRetData = ByteLen_Field * 8 };
enum RetForwardPageType { UseHeap = 0, ForwardFatPtr = 1, UseAuxHeap = 2 };

static const char *GlobHeapMemPtr = "memory_pointer";
static const char *GlobCallDataSize = "calldatasize";
static const char *GlobRetDataSz = "returndatasize";
static const char *GlobCallFlags = "call_flags";
static const char *GlobExtraABIData = "extra_abi_data";
static const char *GlobCallDataPtr = "ptr_calldata";
static const char *GlobRetDataPtr = "ptr_return_data";
static const char *GlobActivePtr = "ptr_active";

enum EntryInfo {
  ArgIndexCallDataABI = 0,
  ArgIndexCallFlags = 1,
  MandatoryArgCnt = 2,
};

class BuilderHelper {
  OpBuilder &b;
  solidity::mlirgen::BuilderHelper h;

public:
  explicit BuilderHelper(OpBuilder &b) : b(b), h(b) {}
  void initGlobs(ModuleOp mod, Location loc) {

    auto initInt = [&](const char *name) {
      LLVM::GlobalOp globOp =
          h.getOrInsertIntGlobalOp(name, mod, AddrSpace_Stack);
      Value globAddr = b.create<LLVM::AddressOfOp>(loc, globOp);
      b.create<LLVM::StoreOp>(loc, h.getConst(0, loc), globAddr,
                              /*alignment=*/32);
    };

    auto i256Ty = b.getIntegerType(256);

    // Initialize the following global ints
    initInt(GlobHeapMemPtr);
    initInt(GlobCallDataSize);
    initInt(GlobRetDataSz);
    initInt(GlobCallFlags);

    // Initialize the GlobExtraABIData int array
    auto extraABIData = h.getOrInsertGlobalOp(
        GlobExtraABIData, mod, LLVM::LLVMArrayType::get(i256Ty, 10),
        /*alignment=*/32, AddrSpace_Stack, LLVM::Linkage::Private,
        b.getZeroAttr(RankedTensorType::get({10}, i256Ty)));
    Value extraABIDataAddr = b.create<LLVM::AddressOfOp>(loc, extraABIData);
    b.create<LLVM::StoreOp>(
        loc,
        h.getConstSplat(std::vector<llvm::APInt>(10, llvm::APInt(256, 0)), loc),
        extraABIDataAddr);
  }

  Value getABILen(Value ptr, Location loc) {
    auto i256Ty = b.getIntegerType(256);

    Value ptrToInt = b.create<LLVM::PtrToIntOp>(loc, i256Ty, ptr).getResult();
    Value lShr = b.create<LLVM::LShrOp>(loc, ptrToInt,
                                        h.getConst(eravm::BitLen_X32 * 3, loc));
    return b.create<LLVM::AndOp>(loc, lShr, h.getConst(UINT_MAX, loc));
  }

  LLVM::LoadOp genLoad(Location loc, Value addr) {
    auto addrOp = llvm::cast<LLVM::AddressOfOp>(addr.getDefiningOp());
    LLVM::GlobalOp globOp = addrOp.getGlobal();
    assert(globOp);
    AddrSpace addrSpace = static_cast<AddrSpace>(globOp.getAddrSpace());
    unsigned alignment =
        addrSpace == AddrSpace_Stack ? ByteLen_Field : ByteLen_Byte;
    return b.create<LLVM::LoadOp>(loc, addrOp, alignment);
  }
};

} // namespace eravm

static LLVM::LLVMFuncOp
getOrInsertLLVMFuncOp(llvm::StringRef name, Type resTy,
                      llvm::ArrayRef<Type> argTys, OpBuilder &b, ModuleOp mod,
                      LLVM::Linkage linkage = LLVM::Linkage::External,
                      llvm::ArrayRef<NamedAttribute> attrs = {}) {
  if (LLVM::LLVMFuncOp found = mod.lookupSymbol<LLVM::LLVMFuncOp>(name))
    return found;

  auto fnType = LLVM::LLVMFunctionType::get(resTy, argTys);

  OpBuilder::InsertionGuard insertGuard(b);
  b.setInsertionPointToStart(mod.getBody());
  return b.create<LLVM::LLVMFuncOp>(mod.getLoc(), name, fnType, linkage,
                                    /*dsoLocal=*/false, LLVM::CConv::C, attrs);
}

static LLVM::LLVMFuncOp getOrInsertCreationFuncOp(llvm::StringRef name,
                                                  Type resTy,
                                                  llvm::ArrayRef<Type> argTys,
                                                  OpBuilder &b, ModuleOp mod) {

  return getOrInsertLLVMFuncOp(
      name, resTy, argTys, b, mod, LLVM::Linkage::Private,
      {NamedAttribute{b.getStringAttr("isRuntime"), b.getBoolAttr(false)}});
}

static LLVM::LLVMFuncOp getOrInsertRuntimeFuncOp(llvm::StringRef name,
                                                 Type resTy,
                                                 llvm::ArrayRef<Type> argTys,
                                                 OpBuilder &b, ModuleOp mod) {

  return getOrInsertLLVMFuncOp(
      name, resTy, argTys, b, mod, LLVM::Linkage::Private,
      {NamedAttribute{b.getStringAttr("isRuntime"), b.getBoolAttr(true)}});
}

static SymbolRefAttr getOrInsertLLVMFuncSym(llvm::StringRef name, Type resTy,
                                            llvm::ArrayRef<Type> argTys,
                                            OpBuilder &b, ModuleOp mod) {
  getOrInsertLLVMFuncOp(name, resTy, argTys, b, mod);
  return SymbolRefAttr::get(mod.getContext(), name);
}

static SymbolRefAttr getOrInsertReturn(PatternRewriter &rewriter,
                                       ModuleOp mod) {
  auto *ctx = mod.getContext();
  auto i256Ty = IntegerType::get(ctx, 256);
  return getOrInsertLLVMFuncSym("__return", LLVM::LLVMVoidType::get(ctx),
                                {i256Ty, i256Ty, i256Ty}, rewriter, mod);
}

/// Returns true if `op` is defined in a runtime context
static bool inRuntimeContext(Operation *op) {
  assert(!isa<LLVM::LLVMFuncOp>(op) && !isa<sol::ObjectOp>(op));

  // Check if the parent FuncOp has isRuntime attribute set
  auto parentFunc = op->getParentOfType<LLVM::LLVMFuncOp>();
  if (parentFunc) {
    auto isRuntimeAttr = parentFunc->getAttr("isRuntime");
    assert(isRuntimeAttr);
    return isRuntimeAttr.cast<BoolAttr>().getValue();
    // TODO: The following doesn't work. Find the rationale (or fix?) for the
    // inconsistent behaviour of llvm::cast and .cast with MLIR data structures
    // return llvm::cast<BoolAttr>(isRuntimeAttr).getValue();
  }

  // If there's no parent FuncOp, check the parent ObjectOp
  auto parentObj = op->getParentOfType<sol::ObjectOp>();
  if (parentObj) {
    return parentObj.getSymName().endswith("_deployed");
  }

  llvm_unreachable("op has no parent FuncOp or ObjectOp");
}

class ReturnOpLowering : public ConversionPattern {
public:
  explicit ReturnOpLowering(MLIRContext *ctx)
      : ConversionPattern(sol::ReturnOp::getOperationName(),
                          /*benefit=*/1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();
    solidity::mlirgen::BuilderHelper b(rewriter);
    auto retOp = cast<sol::ReturnOp>(op);
    auto returnFunc =
        getOrInsertReturn(rewriter, op->getParentOfType<ModuleOp>());

    //
    // Lowering in the runtime context
    //
    if (inRuntimeContext(op)) {
      // Create the return call (__return(offset, length,
      // RetForwardPageType::UseHeap)) and the unreachable op
      rewriter.create<func::CallOp>(
          loc, returnFunc, TypeRange{},
          ValueRange{retOp.getLhs(), retOp.getRhs(),
                     b.getConst(eravm::RetForwardPageType::UseHeap, loc)});
      rewriter.create<LLVM::UnreachableOp>(loc);

      rewriter.eraseOp(op);
      return success();
    }

    //
    // Lowering in the creation context
    //
    auto heapAuxAddrSpacePtrTy = LLVM::LLVMPointerType::get(
        rewriter.getContext(), eravm::AddrSpace_HeapAuxiliary);

    // Store ByteLen_Field to the immutables offset
    auto immutablesOffsetPtr = rewriter.create<LLVM::IntToPtrOp>(
        loc, heapAuxAddrSpacePtrTy,
        b.getConst(eravm::HeapAuxOffsetCtorRetData, loc));
    rewriter.create<LLVM::StoreOp>(loc, b.getConst(eravm::ByteLen_Field, loc),
                                   immutablesOffsetPtr);

    // Store size of immutables in terms of ByteLen_Field to the immutables
    // number offset
    auto immutablesSize = 0; // TODO: Implement this!
    auto immutablesNumPtr = rewriter.create<LLVM::IntToPtrOp>(
        loc, heapAuxAddrSpacePtrTy,
        b.getConst(eravm::HeapAuxOffsetCtorRetData + eravm::ByteLen_Field,
                   loc));
    rewriter.create<LLVM::StoreOp>(
        loc, b.getConst(immutablesSize / eravm::ByteLen_Field, loc),
        immutablesNumPtr);

    // Calculate the return data length (i.e. immutablesSize * 2 +
    // ByteLen_Field * 2
    auto immutablesCalcSize = rewriter.create<arith::MulIOp>(
        loc, b.getConst(immutablesSize, loc), b.getConst(2, loc));
    auto returnDataLen = rewriter.create<arith::AddIOp>(
        loc, immutablesCalcSize.getResult(),
        b.getConst(eravm::ByteLen_Field * 2, loc));

    // Create the return call (__return(HeapAuxOffsetCtorRetData, returnDataLen,
    // RetForwardPageType::UseAuxHeap)) and the unreachable op
    rewriter.create<func::CallOp>(
        loc, returnFunc, TypeRange{},
        ValueRange{b.getConst(eravm::HeapAuxOffsetCtorRetData, loc),
                   returnDataLen.getResult(),
                   b.getConst(eravm::RetForwardPageType::UseAuxHeap, loc)});
    rewriter.create<LLVM::UnreachableOp>(loc);

    rewriter.eraseOp(op);
    return success();
  } // namespace
};

class ObjectOpLowering : public ConversionPattern {
public:
  explicit ObjectOpLowering(MLIRContext *ctx)
      : ConversionPattern(sol::ObjectOp::getOperationName(),
                          /*benefit=*/1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto objOp = cast<sol::ObjectOp>(op);
    auto loc = op->getLoc();
    auto mod = op->getParentOfType<ModuleOp>();
    auto voidTy = LLVM::LLVMVoidType::get(op->getContext());
    auto i256Ty = rewriter.getIntegerType(256);

    // Is this a runtime object?
    // FIXME: Is there a better way to check this?
    if (objOp.getSymName().endswith("_deployed")) {
      // Move the runtime object region under the __runtime function
      auto runtimeFunc =
          getOrInsertRuntimeFuncOp("__runtime", voidTy, {}, rewriter, mod);
      Region &runtimeFuncRegion = runtimeFunc.getRegion();
      rewriter.inlineRegionBefore(objOp.getRegion(), runtimeFuncRegion,
                                  runtimeFuncRegion.begin());
      rewriter.eraseOp(op);
      return success();
    }

    auto genericAddrSpacePtrTy = LLVM::LLVMPointerType::get(
        rewriter.getContext(), eravm::AddrSpace_Generic);

    std::vector<Type> inTys{genericAddrSpacePtrTy};
    constexpr unsigned argCnt = 2 /* Entry::MANDATORY_ARGUMENTS_COUNT */ +
                                10 /* eravm::EXTRA_ABI_DATA_SIZE */;
    for (unsigned i = 0; i < argCnt - 1; ++i) {
      inTys.push_back(i256Ty);
    }
    FunctionType funcType = rewriter.getFunctionType(inTys, {i256Ty});
    rewriter.setInsertionPointToEnd(mod.getBody());
    func::FuncOp entryFunc =
        rewriter.create<func::FuncOp>(loc, "__entry", funcType);
    assert(op->getNumRegions() == 1);

    auto &entryFuncRegion = entryFunc.getRegion();
    Block *entryBlk = rewriter.createBlock(&entryFuncRegion);
    for (auto inTy : inTys) {
      entryBlk->addArgument(inTy, loc);
    }

    rewriter.setInsertionPointToStart(entryBlk);
    solidity::mlirgen::BuilderHelper h(rewriter);
    eravm::BuilderHelper eravmHelper(rewriter);

    // Initialize globals
    eravmHelper.initGlobs(mod, loc);

    // Store the calldata ABI arg to the global calldata ptr
    LLVM::GlobalOp globCallDataPtrDef = h.getOrInsertPtrGlobalOp(
        eravm::GlobCallDataPtr, mod, eravm::AddrSpace_Generic);
    Value globCallDataPtr =
        rewriter.create<LLVM::AddressOfOp>(loc, globCallDataPtrDef);
    rewriter.create<LLVM::StoreOp>(
        loc, entryBlk->getArgument(eravm::EntryInfo::ArgIndexCallDataABI),
        globCallDataPtr, /*alignment=*/32);

    // Store the calldata ABI size to the global calldata size
    Value abiLen = eravmHelper.getABILen(globCallDataPtr, loc);
    LLVM::GlobalOp globCallDataSzDef =
        h.getGlobalOp(eravm::GlobCallDataSize, mod);
    Value globCallDataSz =
        rewriter.create<LLVM::AddressOfOp>(loc, globCallDataSzDef);
    rewriter.create<LLVM::StoreOp>(loc, abiLen, globCallDataSz,
                                   /*alignment=*/32);

    // Store calldatasize[calldata abi arg] to the global ret data ptr and
    // active ptr
    LLVM::LoadOp callDataSz = eravmHelper.genLoad(loc, globCallDataSz);
    auto retDataABIInitializer = rewriter.create<LLVM::GEPOp>(
        loc,
        /*resultType=*/
        LLVM::LLVMPointerType::get(mod.getContext(),
                                   globCallDataPtrDef.getAddrSpace()),
        /*basePtrType=*/rewriter.getIntegerType(eravm::BitLen_Byte),
        entryBlk->getArgument(eravm::EntryInfo::ArgIndexCallDataABI),
        callDataSz.getResult());
    auto storeRetDataABIInitializer = [&](const char *name) {
      LLVM::GlobalOp globDef =
          h.getOrInsertPtrGlobalOp(name, mod, eravm::AddrSpace_Generic);
      Value globAddr = rewriter.create<LLVM::AddressOfOp>(loc, globDef);
      rewriter.create<LLVM::StoreOp>(loc, retDataABIInitializer, globAddr,
                                     /*alignment=*/32);
    };
    storeRetDataABIInitializer(eravm::GlobRetDataPtr);
    storeRetDataABIInitializer(eravm::GlobActivePtr);

    // Store call flags arg to the global call flags
    auto globCallFlagsDef = h.getGlobalOp(eravm::GlobCallFlags, mod);
    Value globCallFlags =
        rewriter.create<LLVM::AddressOfOp>(loc, globCallFlagsDef);
    rewriter.create<LLVM::StoreOp>(
        loc, entryBlk->getArgument(eravm::EntryInfo::ArgIndexCallFlags),
        globCallFlags, /*alignment=*/32);

    // Store the remaining args to the global extra ABI data
    auto globExtraABIDataDef = h.getGlobalOp(eravm::GlobExtraABIData, mod);
    Value globExtraABIData =
        rewriter.create<LLVM::AddressOfOp>(loc, globExtraABIDataDef);
    for (unsigned i = 2; i < entryBlk->getNumArguments(); ++i) {
      auto gep = rewriter.create<LLVM::GEPOp>(
          loc,
          /*resultType=*/
          LLVM::LLVMPointerType::get(mod.getContext(),
                                     globExtraABIDataDef.getAddrSpace()),
          /*basePtrType=*/globExtraABIDataDef.getType(), globExtraABIData,
          ValueRange{h.getConst(0, loc), h.getConst(i, loc)});
      // FIXME: How does the opaque ptr geps with scalar element types lower
      // without explictly setting the elem_type attr?
      gep.setElemTypeAttr(TypeAttr::get(globExtraABIDataDef.getType()));
      rewriter.create<LLVM::StoreOp>(loc, entryBlk->getArgument(i), gep,
                                     /*alignment=*/32);
    }

    // Check Deploy call flag
    auto deployCallFlag = rewriter.create<arith::AndIOp>(
        loc, entryBlk->getArgument(eravm::EntryInfo::ArgIndexCallFlags),
        h.getConst(1, loc));
    auto isDeployCallFlag = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, deployCallFlag.getResult(),
        h.getConst(1, loc));

    // Create the __runtime function
    auto runtimeFunc =
        getOrInsertRuntimeFuncOp("__runtime", voidTy, {}, rewriter, mod);
    Region &runtimeFuncRegion = runtimeFunc.getRegion();
    // Move the runtime object getter under the ObjectOp public API
    for (auto const &op : *objOp.getBody()) {
      if (auto runtimeObj = llvm::dyn_cast<sol::ObjectOp>(&op)) {
        assert(runtimeObj.getSymName().endswith("_deployed"));
        rewriter.inlineRegionBefore(runtimeObj.getRegion(), runtimeFuncRegion,
                                    runtimeFuncRegion.begin());
        rewriter.eraseOp(runtimeObj);
      }
    }

    // Create the __deploy function
    auto deployFunc =
        getOrInsertCreationFuncOp("__deploy", voidTy, {}, rewriter, mod);
    Region &deployFuncRegion = deployFunc.getRegion();
    rewriter.inlineRegionBefore(objOp.getRegion(), deployFuncRegion,
                                deployFuncRegion.begin());

    // If the deploy call flag is set, call __deploy()
    auto ifOp = rewriter.create<scf::IfOp>(loc, isDeployCallFlag.getResult(),
                                           /*withElseRegion=*/true);
    OpBuilder thenBuilder = ifOp.getThenBodyBuilder();
    thenBuilder.create<LLVM::CallOp>(loc, deployFunc, ValueRange{});
    // FIXME: Why the following fails with a "does not reference a valid
    // function" error but generating the func::CallOp to __return is fine
    // thenBuilder.create<func::CallOp>(
    //     loc, SymbolRefAttr::get(mod.getContext(), "__deploy"), TypeRange{},
    //     ValueRange{});

    // Else call __runtime()
    OpBuilder elseBuilder = ifOp.getElseBodyBuilder();
    elseBuilder.create<LLVM::CallOp>(loc, runtimeFunc, ValueRange{});
    rewriter.setInsertionPointAfter(ifOp);
    rewriter.create<LLVM::UnreachableOp>(loc);

    rewriter.eraseOp(op);
    return success();
  }
};

class ContractOpLowering : public ConversionPattern {
public:
  explicit ContractOpLowering(MLIRContext *ctx)
      : ConversionPattern(sol::ContractOp::getOperationName(),
                          /*benefit=*/1, ctx) {}

  LogicalResult
  matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto contOp = cast<sol::ContractOp>(op);
    assert(isa<ModuleOp>(contOp->getParentOp()));
    auto modOp = cast<ModuleOp>(contOp->getParentOp());
    Block *modBody = modOp.getBody();

    // Move functions to the parent ModuleOp
    std::vector<Operation *> funcs;
    for (Operation &func : contOp.getBody()->getOperations()) {
      assert(isa<func::FuncOp>(&func));
      funcs.push_back(&func);
    }
    for (Operation *func : funcs) {
      func->moveAfter(modBody, modBody->begin());
    }

    rewriter.eraseOp(op);
    return success();
  }
};

struct SolidityDialectLowering
    : public PassWrapper<SolidityDialectLowering, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SolidityDialectLowering)

  void getDependentDialects(DialectRegistry &reg) const override {
    reg.insert<LLVM::LLVMDialect, func::FuncDialect, arith::ArithmeticDialect,
               scf::SCFDialect>();
  }

  void runOnOperation() override {
    LLVMConversionTarget llConv(getContext());
    llConv.addLegalOp<ModuleOp>();
    llConv.addLegalOp<scf::YieldOp>();
    LLVMTypeConverter llTyConv(&getContext());

    RewritePatternSet pats(&getContext());
    arith::populateArithmeticToLLVMConversionPatterns(llTyConv, pats);
    populateMemRefToLLVMConversionPatterns(llTyConv, pats);
    populateSCFToControlFlowConversionPatterns(pats);
    cf::populateControlFlowToLLVMConversionPatterns(llTyConv, pats);
    populateFuncToLLVMConversionPatterns(llTyConv, pats);
    pats.add<ObjectOpLowering, ReturnOpLowering>(&getContext());

    ModuleOp mod = getOperation();
    if (failed(applyFullConversion(mod, llConv, std::move(pats))))
      signalPassFailure();
  }
};

} // namespace

std::unique_ptr<Pass> sol::createSolidityDialectLoweringPassForEraVM() {
  return std::make_unique<SolidityDialectLowering>();
}
