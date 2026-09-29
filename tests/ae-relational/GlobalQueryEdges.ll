target datalayout = "e-m:o-p:64:64-i64:64-n32:64-S128"
@arr = global [4 x i32] zeroinitializer
@negative = global ptr getelementptr (i8, ptr @arr, i64 -1)
@outside = global ptr getelementptr (i8, ptr @arr, i64 17)
@ext = external global [0 x i32]
@unknown = global ptr getelementptr (i8, ptr @ext, i64 8)
define i32 @main() {
  %first = load i32, ptr @arr
  %last = getelementptr i8, ptr @arr, i64 12
  %good = load i32, ptr %last
  %end = getelementptr i8, ptr @arr, i64 16
  store i32 1, ptr %end
  %p = load ptr, ptr @unknown
  %unmodeled = load i32, ptr %p
  ret i32 0
}
