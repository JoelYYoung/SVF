; Deliberately violates the external MEMCPY model signature. Must fail closed.
@source = constant [4 x i8] c"abc\00"
declare ptr @__memcpy_chk(i64, ptr, i64, i64)
define i32 @main() {
  %r = call ptr @__memcpy_chk(i64 1, ptr @source, i64 4, i64 4)
  ret i32 0
}
