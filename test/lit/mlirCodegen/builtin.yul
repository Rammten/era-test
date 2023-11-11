// RUN: solc --yul --mlir %s | FileCheck %s
// RUN: solc --yul --mlir --mmlir --mlir-print-debuginfo %s | FileCheck --check-prefix=DBG %s

{
  mstore(0, 42)
  return(0, 0)
}
// NOTE: Assertions have been autogenerated by test/updFileCheckTest.py
// CHECK: module {
// CHECK-NEXT:   sol.object @object {
// CHECK-NEXT:     %c0_i256 = arith.constant 0 : i256
// CHECK-NEXT:     %c42_i256 = arith.constant 42 : i256
// CHECK-NEXT:     "sol.mstore"(%c0_i256, %c42_i256) : (i256, i256) -> ()
// CHECK-NEXT:     %c0_i256_0 = arith.constant 0 : i256
// CHECK-NEXT:     %c0_i256_1 = arith.constant 0 : i256
// CHECK-NEXT:     "sol.return"(%c0_i256_0, %c0_i256_1) : (i256, i256) -> ()
// CHECK-NEXT:   }
// CHECK-NEXT: }
// DBG: module {
// DBG-NEXT:   sol.object @object {
// DBG-NEXT:     %c0_i256 = arith.constant 0 : i256 loc(#loc1)
// DBG-NEXT:     %c42_i256 = arith.constant 42 : i256 loc(#loc2)
// DBG-NEXT:     "sol.mstore"(%c0_i256, %c42_i256) : (i256, i256) -> () loc(#loc3)
// DBG-NEXT:     %c0_i256_0 = arith.constant 0 : i256 loc(#loc4)
// DBG-NEXT:     %c0_i256_1 = arith.constant 0 : i256 loc(#loc5)
// DBG-NEXT:     "sol.return"(%c0_i256_0, %c0_i256_1) : (i256, i256) -> () loc(#loc6)
// DBG-NEXT:   } loc(#loc0)
// DBG-NEXT: } loc(#loc0)
// DBG-NEXT: #loc0 = loc(unknown)
// DBG-NEXT: #loc1 = loc({{.*}}:4:9)
// DBG-NEXT: #loc2 = loc({{.*}}:4:12)
// DBG-NEXT: #loc3 = loc({{.*}}:4:2)
// DBG-NEXT: #loc4 = loc({{.*}}:5:9)
// DBG-NEXT: #loc5 = loc({{.*}}:5:12)
// DBG-NEXT: #loc6 = loc({{.*}}:5:2)
