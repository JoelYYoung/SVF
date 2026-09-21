; Without caller bounds, increment may wrap at INT_MAX. Actual/formal and
; return relations must not turn the feasible result <= argc path into Safe.
source_filename = "relational-call-wrap-witness.c"
define i32 @increment(i32 %x) {
entry:
  %y = add i32 %x, 1
  ret i32 %y
}
define i32 @main(i32 %argc, ptr %argv) {
entry:
  %result = call i32 @increment(i32 %argc)
  %wrap = icmp sle i32 %result, %argc
  br i1 %wrap, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
