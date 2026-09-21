; The configured recursion fallback is conservative.  Even though this small
; recurrence returns x for non-negative x, an unchecked recursive summary must
; not certify the result == x property and remove the possible bad branch.

source_filename = "recursive-call-witness.c"

define i32 @countdown(i32 %x) {
entry:
  %base = icmp sle i32 %x, 0
  br i1 %base, label %done, label %step

step:
  %next = sub i32 %x, 1
  %inner = call i32 @countdown(i32 %next)
  %result = add i32 %inner, 1
  ret i32 %result

done:
  ret i32 0
}

define i32 @main(i32 %x, ptr %argv) {
entry:
  %lower = icmp sge i32 %x, 0
  br i1 %lower, label %guard.upper, label %exit

guard.upper:
  %upper = icmp sle i32 %x, 3
  br i1 %upper, label %call, label %exit

call:
  %value = call i32 @countdown(i32 %x)
  %possible = icmp ne i32 %value, %x
  br i1 %possible, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
