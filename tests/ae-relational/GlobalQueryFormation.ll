target datalayout = "e-m:o-p:64:64-i64:64-n32:64-S128"
@arr = global [4 x i32] zeroinitializer
@inside = global ptr getelementptr (i8, ptr @arr, i64 12)
@end = global ptr getelementptr (i8, ptr @arr, i64 16)
@s = global { [4 x i32], i32 } zeroinitializer
@field = global ptr getelementptr (i8, ptr @s, i64 16)
@extern_arr = external global [0 x i32]
@unknown = global ptr getelementptr (i8, ptr @extern_arr, i64 8)
define i32 @main() {
  ret i32 0
}
