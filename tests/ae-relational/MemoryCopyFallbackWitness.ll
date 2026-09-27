; Partial, zero, optional-length and ambiguous-destination copies.
; Q0 May, Q1 Safe, Q2 May, Q3 May.
source_filename = "MemoryCopyFallbackWitness.ll"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
@o = global i32 0, align 4
@q = global i32 0, align 4
@v = global i32 7, align 4
declare i1 @choice()
declare void @svf_assert(i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
define i32 @main() {
entry:
  store i32 1, ptr @o
  call void @llvm.memcpy.p0.p0.i64(ptr @o, ptr @v, i64 1, i1 false)
  %a = load i32, ptr @o
  %stale = icmp eq i32 %a, 1
  call void @svf_assert(i1 %stale)
  store i32 1, ptr @o
  call void @llvm.memcpy.p0.p0.i64(ptr @o, ptr @v, i64 0, i1 false)
  %b = load i32, ptr @o
  %zero = icmp eq i32 %b, 1
  call void @svf_assert(i1 %zero)
  %c = call i1 @choice()
  %n = select i1 %c, i64 0, i64 4
  call void @llvm.memcpy.p0.p0.i64(ptr @o, ptr @v, i64 %n, i1 false)
  %d = load i32, ptr @o
  %optional = icmp eq i32 %d, 1
  call void @svf_assert(i1 %optional)
  store i32 1, ptr @o
  %p = select i1 %c, ptr @o, ptr @q
  call void @llvm.memcpy.p0.p0.i64(ptr %p, ptr @v, i64 4, i1 false)
  %e = load i32, ptr @o
  %ambiguous = icmp eq i32 %e, 7
  call void @svf_assert(i1 %ambiguous)
  ret i32 0
}
