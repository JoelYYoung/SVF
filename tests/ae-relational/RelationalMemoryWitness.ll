; A singleton strong store/load pair should preserve y = x through the
; abstract memory content variable.
source_filename = "relational-memory-witness.c"
define i32 @main(i32 %x, ptr %argv) {
entry:
  %slot = alloca i32, align 4
  store i32 %x, ptr %slot, align 4
  %y = load i32, ptr %slot, align 4
  %impossible = icmp slt i32 %y, %x
  br i1 %impossible, label %bad, label %exit
bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault
exit:
  ret i32 0
}
