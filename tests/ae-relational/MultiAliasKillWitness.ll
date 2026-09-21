; The selected pointer may designate either slot.  A store through it is a
; weak, multi-target update and must kill the old a = x relation on the a path.

source_filename = "multi-alias-kill-witness.c"

declare i1 @nondet_i1()
declare i32 @nondet_i32()

define i32 @main(i32 %x, ptr %argv) {
entry:
  %a = alloca i32, align 4
  %b = alloca i32, align 4
  store i32 %x, ptr %a, align 4
  store i32 %x, ptr %b, align 4
  %condition = call i1 @nondet_i1()
  %pointer = select i1 %condition, ptr %a, ptr %b
  %unknown = call i32 @nondet_i32()
  store i32 %unknown, ptr %pointer, align 4
  %value = load i32, ptr %a, align 4
  %possible = icmp ne i32 %value, %x
  br i1 %possible, label %bad, label %exit

bad:
  %fault = load i32, ptr null, align 4
  ret i32 %fault

exit:
  ret i32 0
}
