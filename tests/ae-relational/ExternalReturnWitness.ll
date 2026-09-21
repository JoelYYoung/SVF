; A registered external handler may model only memory side effects. Its
; non-void SSA result is still defined and must be initialized Top in every
; storage mode. The zero branch remains feasible and therefore keeps the null
; load as a May alarm; dense and semi-sparse Post replay must agree.

source_filename = "external-return-witness.c"

declare i64 @fread(ptr, i64, i64, ptr)

define i32 @main() {
entry:
  %count = call i64 @fread(ptr null, i64 1, i64 1, ptr null)
  %is.zero = icmp eq i64 %count, 0
  br i1 %is.zero, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
