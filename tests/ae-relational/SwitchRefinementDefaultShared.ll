; A destination contains both default and case 1: it must not narrow to 1.
@g = global i32 0
declare i32 @nondet_i32()
declare void @svf_assert(i1)
define i32 @main() {
entry:
  %x = call i32 @nondet_i32()
  store i32 %x, ptr @g
  %y = load i32, ptr @g
  switch i32 %y, label %default [i32 1, label %default
                               i32 2, label %two]
two:
  ret i32 0
default:
  %z = load i32, ptr @g
  %c = icmp eq i32 %z, 1
  call void @svf_assert(i1 %c)
  ret i32 0
}
