; The branch is feasible for argc == INT_MAX because LLVM add wraps. A sound
; relational transfer must not retain the unbounded-integer equation y=x+1.
source_filename = "relational-wrap-witness.c"
define i32 @main(i32 %argc, ptr %argv) {
entry:
  %y = add i32 %argc, 1
  %wrap = icmp sle i32 %y, %argc
  br i1 %wrap, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
