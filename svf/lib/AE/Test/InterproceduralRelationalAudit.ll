; Audit fixture for a relation that must cross a call boundary.
;
; Caller establishes %actual_y == %actual_x.  The callee can prove
; %formal_a - %formal_b == 0 only if the call transformer simultaneously
; renames both actuals to both formals while retaining their relation.

define i32 @difference(i32 %formal_a, i32 %formal_b) {
entry:
  %formal_difference = sub nsw i32 %formal_a, %formal_b
  ret i32 %formal_difference
}

define i32 @main(i32 %actual_x, ptr %argv) {
entry:
  %actual_y = add nsw i32 %actual_x, 0
  %call_result = call i32 @difference(i32 %actual_x, i32 %actual_y)
  %result_is_zero = icmp eq i32 %call_result, 0
  br i1 %result_is_zero, label %ok, label %failure

failure:
  %interproc_bad = add nsw i32 %call_result, 1000
  ret i32 %interproc_bad

ok:
  ret i32 0
}
