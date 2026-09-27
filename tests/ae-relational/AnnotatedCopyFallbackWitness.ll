; A MEMCPY annotation alone does not mean whole-value memcpy semantics.
; strncpy zero-pads after the first NUL; memccpy's third argument is a byte,
; not its length. Both assertions below must be May.
source_filename = "AnnotatedCopyFallbackWitness.ll"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
@o = global i32 1, align 4
@v = global i32 305397760, align 4
declare ptr @strncpy(ptr, ptr, i64)
declare ptr @memccpy(ptr, ptr, i32, i64)
declare void @svf_assert(i1)
define i32 @main() {
entry:
  %r = call ptr @strncpy(ptr @o, ptr @v, i64 4)
  %a = load i32, ptr @o
  %bad_full_value = icmp eq i32 %a, 305397760
  call void @svf_assert(i1 %bad_full_value)
  store i32 1, ptr @o
  store i32 7, ptr @v
  %s = call ptr @memccpy(ptr @o, ptr @v, i32 0, i64 4)
  %b = load i32, ptr @o
  %bad_no_write = icmp eq i32 %b, 1
  call void @svf_assert(i1 %bad_no_write)
  ret i32 0
}
