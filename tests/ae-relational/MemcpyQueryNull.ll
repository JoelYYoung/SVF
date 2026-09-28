; Negative analysis fixture: do not execute.
@source = constant [4 x i8] c"abc\00"
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare ptr @__memcpy_chk(ptr, ptr, i64, i64)
define i32 @main() {
  call void @llvm.memcpy.p0.p0.i64(ptr null, ptr @source, i64 4, i1 false)
  %result = call ptr @__memcpy_chk(ptr null, ptr @source, i64 4, i64 4)
  ret i32 0
}
