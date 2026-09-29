target datalayout = "e-m:o-p:64:64-i64:64-n32:64-S128"
@arr = global [4 x i32] zeroinitializer
@end = global ptr getelementptr (i8, ptr @arr, i64 16)
define i32 @main() {
  %onepast = load ptr, ptr @end
  %bad = load i32, ptr %onepast
  ret i32 %bad
}
