; Unknown integers remain numerical Top. Converting one to a pointer must
; conservatively produce Address Top, while integer zero produces null.

define i32 @main(i64 %unknown_integer) {
entry:
  %unknown_pointer = inttoptr i64 %unknown_integer to ptr
  %null_pointer = inttoptr i64 0 to ptr
  ret i32 0
}
