// RUN: solc --yul --mlir-action=print-init %s | FileCheck %s
// RUN: solc --yul --mlir-action=print-init --mmlir --mlir-print-debuginfo %s | FileCheck --check-prefix=DBG %s

{
}
// NOTE: Assertions have been autogenerated by test/updFileCheckTest.py
// CHECK: module {
// CHECK-NEXT:   sol.object @object {
// CHECK-NEXT:   }
// CHECK-NEXT: }
// CHECK-EMPTY:
// DBG: module {
// DBG-NEXT:   sol.object @object {
// DBG-NEXT:   } loc(#loc)
// DBG-NEXT: } loc(#loc)
// DBG-NEXT: #loc = loc(unknown)
// DBG-EMPTY: