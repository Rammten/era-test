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
// Solidity to MLIR pass
//

#include "Solidity/SolidityOps.h"
#include "liblangutil/CharStream.h"
#include "liblangutil/Exceptions.h"
#include "liblangutil/SourceLocation.h"
#include "libsolidity/ast/AST.h"
#include "libsolidity/ast/ASTVisitor.h"
#include "libsolidity/codegen/mlir/Interface.h"
#include "libsolidity/codegen/mlir/Passes.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/AsmState.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/PassManager.h"
#include "range/v3/view/zip.hpp"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace solidity::langutil;
using namespace solidity::frontend;

namespace solidity::frontend {

class SolidityToMLIRPass : public ASTConstVisitor {
public:
  explicit SolidityToMLIRPass(mlir::MLIRContext &ctx, CharStream const &stream)
      : b(&ctx), stream(stream) {
    mod = mlir::ModuleOp::create(b.getUnknownLoc());
    b.setInsertionPointToEnd(mod.getBody());
  }

  void run(ContractDefinition const &);

  /// Returns the ModuleOp
  mlir::ModuleOp getModule() { return mod; }

private:
  mlir::OpBuilder b;
  CharStream const &stream;
  mlir::ModuleOp mod;

  /// The function being lowered
  FunctionDefinition const *currFunc;

  // TODO: Use VariableDeclaration instead?
  /// Maps variables to its MemRef
  std::map<Declaration const *, mlir::Value> varMemRef;

  /// Returns the mlir location for the solidity source location `loc`
  mlir::Location loc(SourceLocation const &loc) {
    // FIXME: Track loc.end as well
    LineColumn lineCol = stream.translatePositionToLineColumn(loc.start);
    return mlir::FileLineColLoc::get(b.getStringAttr(stream.name()),
                                     lineCol.line, lineCol.column);
  }

  /// Returns the corresponding mlir type for the solidity type `ty`
  mlir::Type type(Type const *ty);

  /// Returns the MemRef of the variable
  mlir::Value getMemRef(Declaration const *decl);
  mlir::Value getMemRef(Identifier const *ident);

  /// Sets the MemRef of the variable
  void setMemRef(Declaration const *decl, mlir::Value addr);

  /// Returns the cast from `val` having the corresponding mlir type of
  /// `srcTy` to a value having the corresponding mlir type of `dstTy`
  mlir::Value genCast(mlir::Value val, Type const *srcTy, Type const *dstTy);

  /// Returns the mlir expression for the literal `lit`
  mlir::Value genExpr(Literal const *lit);

  /// Returns the mlir expression for the binary operation `binOp`
  mlir::Value genExpr(BinaryOperation const *binOp);

  /// Returns the mlir expression from `expr` and optionally casts it to the
  /// corresponding mlir type of `resTy`
  mlir::Value genExpr(Expression const *expr,
                      std::optional<Type const *> resTy = std::nullopt);

  bool visit(Return const &) override;
  void run(FunctionDefinition const &);
};

} // namespace solidity::frontend

mlir::Type SolidityToMLIRPass::type(Type const *ty) {
  // Integer type
  if (auto *i = dynamic_cast<IntegerType const *>(ty)) {
    return b.getIntegerType(i->numBits());
  }
  // Rational number type
  else if (auto *ratNumTy = dynamic_cast<RationalNumberType const *>(ty)) {
    // TODO:
    if (ratNumTy->isFractional())
      solUnimplemented("Unhandled type\n");

    // Integral rational number type
    const IntegerType *intTy = ratNumTy->integerType();
    return b.getIntegerType(intTy->numBits());
  }
  // TODO:
  solUnimplemented("Unhandled type\n");
}

mlir::Value SolidityToMLIRPass::getMemRef(Declaration const *decl) {
  return varMemRef[decl];
}

void SolidityToMLIRPass::setMemRef(Declaration const *decl, mlir::Value addr) {
  varMemRef[decl] = addr;
}

mlir::Value SolidityToMLIRPass::getMemRef(Identifier const *ident) {
  return getMemRef(ident->annotation().referencedDeclaration);
}

mlir::Value SolidityToMLIRPass::genCast(mlir::Value val, Type const *srcTy,
                                        Type const *dstTy) {
  // Don't cast if we're casting to the same type
  if (srcTy == dstTy)
    return val;

  auto getAsIntTy = [](Type const *ty) -> IntegerType const * {
    auto intTy = dynamic_cast<IntegerType const *>(ty);
    if (!intTy) {
      if (auto *ratTy = dynamic_cast<RationalNumberType const *>(ty)) {
        if (auto *intRatTy = ratTy->integerType())
          return intRatTy;
      }
      return nullptr;
    }
    return intTy;
  };

  // We generate signless integral mlir::Types, so we must track the solidity
  // type to perform "sign aware lowering".
  //
  // Casting between integers
  auto srcIntTy = getAsIntTy(srcTy);
  auto dstIntTy = getAsIntTy(dstTy);

  if (srcIntTy && dstIntTy) {
    // Generate extends
    if (dstIntTy->numBits() > srcIntTy->numBits()) {
      return dstIntTy->isSigned()
                 ? b.create<mlir::arith::ExtSIOp>(val.getLoc(), type(dstIntTy),
                                                  val)
                       ->getResult(0)
                 : b.create<mlir::arith::ExtUIOp>(val.getLoc(), type(dstIntTy),
                                                  val)
                       ->getResult(0);
    } else {
      // TODO:
      solUnimplemented("Unhandled cast\n");
    }
  }

  // TODO:
  solUnimplemented("Unhandled cast\n");
}

mlir::Value SolidityToMLIRPass::genExpr(BinaryOperation const *binOp) {
  auto resTy = binOp->annotation().type;
  auto lc = loc(binOp->location());

  mlir::Value lhs = genExpr(&binOp->leftExpression(), resTy);
  mlir::Value rhs = genExpr(&binOp->rightExpression(), resTy);

  switch (binOp->getOperator()) {
  case Token::Add:
    return b.create<mlir::arith::AddIOp>(lc, lhs, rhs)->getResult(0);
  case Token::Mul:
    return b.create<mlir::arith::MulIOp>(lc, lhs, rhs)->getResult(0);
  default:
    break;
  }
  solUnimplemented("Unhandled binary operation");
}

mlir::Value SolidityToMLIRPass::genExpr(Expression const *expr,
                                        std::optional<Type const *> resTy) {
  mlir::Value val;

  // Generate literals
  if (auto *lit = dynamic_cast<Literal const *>(expr)) {
    val = genExpr(lit);
  }
  // Generate variable access
  else if (auto *ident = dynamic_cast<Identifier const *>(expr)) {
    return b.create<mlir::memref::LoadOp>(loc(expr->location()),
                                          getMemRef(ident));
  }
  // Generate binary operation
  else if (auto *binOp = dynamic_cast<BinaryOperation const *>(expr)) {
    val = genExpr(binOp);
  }

  // Generate cast (Optional)
  if (resTy) {
    return genCast(val, expr->annotation().type, *resTy);
  }

  return val;
}

mlir::Value SolidityToMLIRPass::genExpr(Literal const *lit) {
  mlir::Location lc = loc(lit->location());
  Type const *ty = lit->annotation().type;

  // Rational number literal
  if (auto *ratNumTy = dynamic_cast<RationalNumberType const *>(ty)) {
    // TODO:
    if (ratNumTy->isFractional())
      solUnimplemented("Unhandled literal\n");

    auto *intTy = ratNumTy->integerType();
    u256 val = ty->literalValue(lit);
    // TODO: Is there a faster way to convert boost::multiprecision::number to
    // llvm::APInt?
    return b.create<mlir::arith::ConstantOp>(
        lc, b.getIntegerAttr(type(ty), llvm::APInt(intTy->numBits(), val.str(),
                                                   /*radix=*/10)));
  } else {
    // TODO:
    solUnimplemented("Unhandled literal\n");
  }
}

bool SolidityToMLIRPass::visit(Return const &ret) {
  auto currFuncResTys =
      currFunc->functionType(/*FIXME*/ true)->returnParameterTypes();

  // The function generator emits `ReturnOp` for empty result
  if (currFuncResTys.empty())
    return true;

  solUnimplementedAssert(currFuncResTys.size() == 1,
                         "TODO: Impl multivalued return");

  Expression const *astExpr = ret.expression();
  if (astExpr) {
    mlir::Value expr = genExpr(ret.expression(), currFuncResTys[0]);
    b.create<mlir::func::ReturnOp>(loc(ret.location()), expr);
  } else {
    solUnimplementedAssert(false, "NYI: Empty return");
  }

  return true;
}

void SolidityToMLIRPass::run(FunctionDefinition const &func) {
  currFunc = &func;
  std::vector<mlir::Type> inpTys, outTys;
  std::vector<mlir::Location> inpLocs;

  for (auto const &param : func.parameters()) {
    inpTys.push_back(type(param->annotation().type));
    inpLocs.push_back(loc(param->location()));
  }

  for (auto const &param : func.returnParameters()) {
    outTys.push_back(type(param->annotation().type));
  }

  // TODO:
  solUnimplementedAssert(outTys.size() <= 1, "TODO: Impl multivalued return");

  // TODO: Specify visibility
  auto funcType = b.getFunctionType(inpTys, outTys);
  auto op =
      b.create<mlir::func::FuncOp>(loc(func.location()), func.name(), funcType);

  mlir::Block *entryBlk = b.createBlock(&op.getRegion());
  b.setInsertionPointToStart(entryBlk);

  for (auto &&[inpTy, inpLoc, param] :
       ranges::views::zip(inpTys, inpLocs, func.parameters())) {
    mlir::Value arg = entryBlk->addArgument(inpTy, inpLoc);
    // TODO: Support non-scalars
    mlir::MemRefType memRefTy = mlir::MemRefType::get({}, inpTy);
    mlir::Value addr =
        b.create<mlir::memref::AllocaOp>(inpLoc, memRefTy).getResult();
    setMemRef(param.get(), addr);
    b.create<mlir::memref::StoreOp>(inpLoc, arg, addr);
  }

  func.accept(*this);

  // Generate empty return
  if (outTys.empty())
    b.create<mlir::func::ReturnOp>(loc(func.location()));

  b.setInsertionPointAfter(op);
}

void SolidityToMLIRPass::run(ContractDefinition const &cont) {
  auto op = b.create<mlir::sol::ContractOp>(loc(cont.location()), cont.name());
  b.setInsertionPointToStart(op.getBody());

  for (auto *f : cont.definedFunctions()) {
    run(*f);
  }
  b.setInsertionPointAfter(op);
}

bool solidity::mlirgen::runSolidityToMLIRPass(
    std::vector<ContractDefinition const *> const &contracts,
    CharStream const &stream, solidity::mlirgen::JobSpec const &job) {
  mlir::MLIRContext ctx;
  ctx.getOrLoadDialect<mlir::sol::SolidityDialect>();
  ctx.getOrLoadDialect<mlir::func::FuncDialect>();
  ctx.getOrLoadDialect<mlir::arith::ArithmeticDialect>();
  ctx.getOrLoadDialect<mlir::memref::MemRefDialect>();

  SolidityToMLIRPass gen(ctx, stream);
  for (auto *contract : contracts) {
    gen.run(*contract);
  }
  mlir::ModuleOp mod = gen.getModule();

  if (failed(mlir::verify(mod))) {
    mod.emitError("Module verification error");
    return false;
  }

  return doJob(job, ctx, mod);
}

void solidity::mlirgen::registerMLIRCLOpts() {
  mlir::registerAsmPrinterCLOptions();
}

bool solidity::mlirgen::parseMLIROpts(std::vector<const char *> &argv) {
  // ParseCommandLineOptions() expects argv[0] to be the name of a program
  std::vector<const char *> fooArgv{"foo"};
  for (const char *arg : argv) {
    fooArgv.push_back(arg);
  }

  return llvm::cl::ParseCommandLineOptions(fooArgv.size(), fooArgv.data(),
                                           "Generic MLIR flags\n");
}
