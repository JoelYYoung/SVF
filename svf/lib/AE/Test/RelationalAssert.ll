; End-to-end abstract-interpreter fixture for:
;
;   a = rand()
;   x = a
;   if (a > 0)
;       assert(x > 0)
;
; The argument %a models an arbitrary integer input. Interval analysis alone
; cannot push the branch refinement from %a to the already copied %assert_x.
; Octagon retains %assert_x == %a, so the assertion-failure edge is bottom.

define i32 @main(i32 %a, ptr %argv) {
entry:
  %assert_x = add nsw i32 %a, 0
  %a_is_positive = icmp sgt i32 %a, 0
  br i1 %a_is_positive, label %check_assertion, label %exit

check_assertion:
  %assert_condition = icmp sgt i32 %assert_x, 0
  br i1 %assert_condition, label %assert_ok, label %assert_fail

assert_fail:
  %assert_bad = add nsw i32 %assert_x, 1000
  ret i32 %assert_bad

assert_ok:
  ret i32 %assert_x

exit:
  ret i32 0
}
