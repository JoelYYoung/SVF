@g = global i32 7
declare void @svf_assert(i1)
define i32 @main() {
entry:
  %conditionValue = load i32, ptr @g
  %condition = icmp eq i32 %conditionValue, 7
  br i1 %condition, label %live, label %dead
dead:
  br label %merge
live:
  %x = load i32, ptr @g
  %ok = icmp eq i32 %x, 7
  call void @svf_assert(i1 %ok)
  br label %merge
merge:
  ret i32 0
}
