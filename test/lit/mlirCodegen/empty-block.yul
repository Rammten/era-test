// RUN: solc --yul --mlir %s | FileCheck %s
// RUN: solc --yul --mlir --mmlir --mlir-print-debuginfo %s | FileCheck --check-prefix=DBG %s

{
}
// NOTE: Assertions have been autogenerated by test/updFileCheckTest.py
// CHECK: module {
// CHECK-NEXT:   sol.object @object {
// CHECK-NEXT:   }
// CHECK-NEXT: }
// DBG: module {
// DBG-NEXT:   sol.object @object {
// DBG-NEXT:   } loc(#loc)
// DBG-NEXT: } loc(#loc)
// DBG-NEXT: #loc = loc(unknown)
