declare i32 @puts(ptr)

@.str.0 = private unnamed_addr constant [30 x i8] c"Part 2: stdio library example\00"

define i32 @main() {
entry:
  call i32 @puts(ptr @.str.0)
  ret i32 0
}
