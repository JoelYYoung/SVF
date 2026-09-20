; Small registration contract input; synthetic late/interleaved objects are
; constructed by the observer, not by production analysis.
define i32 @main() {
entry:
  %group_a = alloca [4 x i32]
  %group_b = alloca [4 x i32]
  %value = alloca i32
  %pointer = alloca ptr
  store i32 7, ptr %value
  store ptr %value, ptr %pointer
  %p = load ptr, ptr %pointer
  %v = load i32, ptr %p
  ret i32 %v
}
