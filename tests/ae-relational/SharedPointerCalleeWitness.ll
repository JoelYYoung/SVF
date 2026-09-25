; CallPE is a phi-like merge whose pointer operands live at distinct call
; sites. Post replay must reconstruct each actual pointer at its annotated
; call node before validating the shared callee-entry equation.
source_filename = "shared-pointer-callee-witness.c"

define void @read_value(ptr %pointer) {
entry:
  %value = load i32, ptr %pointer, align 4
  ret void
}

define i32 @main(i32 %selector, ptr %argv) {
entry:
  %left.slot = alloca i32, align 4
  %right.slot = alloca i32, align 4
  store i32 11, ptr %left.slot, align 4
  store i32 22, ptr %right.slot, align 4
  %choose = icmp eq i32 %selector, 0
  br i1 %choose, label %left, label %right

left:
  call void @read_value(ptr %left.slot)
  br label %exit

right:
  call void @read_value(ptr %right.slot)
  br label %exit

exit:
  ret i32 0
}
