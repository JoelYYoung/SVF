; Defined pointer -> pointer-sized integer -> pointer round trip.
; Q0 must be May: the store changes o from 1 to 7. Q1 is concretely true.
source_filename = "PointerRoundTripStoreWitness.ll"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
@o = global i32 0, align 4
declare void @svf_assert(i1)
define i32 @main() {
entry:
  store i32 1, ptr @o
  %i = ptrtoint ptr @o to i64
  %p = inttoptr i64 %i to ptr
  store i32 7, ptr %p
  %y = load i32, ptr @o
  %stale = icmp eq i32 %y, 1
  call void @svf_assert(i1 %stale)
  %changed = icmp eq i32 %y, 7
  call void @svf_assert(i1 %changed)
  ret i32 0
}
