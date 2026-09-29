; Unknown i128 switch: neither the case nor default may assume a signed i64 label.
@g = global i128 0
declare i128 @unknown_wide()
declare void @svf_assert(i1)
define i32 @main() {
entry:
  %x = call i128 @unknown_wide()
  store i128 %x, ptr @g
  %y = load i128, ptr @g
  switch i128 %y, label %default [i128 18446744073709551616, label %case]
case:
  %a = load i128, ptr @g
  %ac = icmp eq i128 %a, -1
  call void @svf_assert(i1 %ac)
  ret i32 0
default:
  %b = load i128, ptr @g
  %bc = icmp eq i128 %b, -1
  call void @svf_assert(i1 %bc)
  ret i32 0
}
