; An intraprocedural call boundary must discard callee relations without
; discarding caller-local relations between immutable SSA values.

source_filename = "relational-caller-frame-witness.c"

define void @observe(i32 %value) {
entry:
  %nonnegative = icmp sge i32 %value, 0
  ret void
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %lower = icmp sge i32 %argc, 0
  br i1 %lower, label %guard.upper, label %exit

guard.upper:
  %upper = icmp sle i32 %argc, 10
  br i1 %upper, label %compute, label %exit

compute:
  %next = add i32 %argc, 1
  call void @observe(i32 %argc)
  %impossible = icmp sle i32 %next, %argc
  br i1 %impossible, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
