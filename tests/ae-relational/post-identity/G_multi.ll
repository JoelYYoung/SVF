; Expected negative semantic coverage gate: two roots remain Unsupported.
; Identity must still be present, unique and independent of node numbering.
@g = global i32 0
define i32 @root_a() {
entry:
  %a = load i32, ptr @g
  ret i32 %a
}
define i32 @root_b() {
entry:
  %b = load i32, ptr @g
  ret i32 %b
}
