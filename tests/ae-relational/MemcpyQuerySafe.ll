@source = constant [4 x i8] c"abc\00"
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare ptr @__memcpy_chk(ptr, ptr, i64, i64)
define i32 @main() {
  %destination = alloca [4 x i8], align 1
  call void @llvm.memcpy.p0.p0.i64(ptr %destination, ptr @source, i64 4, i1 false)
  %result = call ptr @__memcpy_chk(ptr %destination, ptr @source, i64 4, i64 4)
  ret i32 0
}
