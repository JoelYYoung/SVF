; Tests actual/formal and return bindings. The caller bounds make the callee
; add non-wrapping, and the returned value remains argc + 1.

source_filename = "relational-call-witness.c"

define i32 @increment(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}

define i32 @main(i32 %argc, ptr %argv) {
entry:
  %lower = icmp sge i32 %argc, 0
  br i1 %lower, label %guard.upper, label %exit

guard.upper:
  %upper = icmp sle i32 %argc, 10
  br i1 %upper, label %call, label %exit

call:
  %result = call i32 @increment(i32 %argc)
  %impossible = icmp sle i32 %result, %argc
  br i1 %impossible, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
