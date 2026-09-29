; Both cases are concretely reachable: 200 has the i8 bit pattern of -56.
@g = global i8 0
declare void @svf_assert(i1)
define i32 @main() {
entry:
  %x = trunc i32 200 to i8
  switch i8 %x, label %next [i8 -56, label %trunc_case]
trunc_case:
  call void @svf_assert(i1 false)
  br label %next
next:
  store i8 %x, ptr @g
  %y = load i8, ptr @g
  switch i8 %y, label %end [i8 -56, label %load_case]
load_case:
  call void @svf_assert(i1 false)
  br label %end
end:
  ret i32 0
}
