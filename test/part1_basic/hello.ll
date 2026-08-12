declare i32 @puts(ptr)

@.str.0 = private unnamed_addr constant [36 x i8] c"Part 1: basic ThaiThon import works\00"

define i32 @main() {
entry:
  call i32 @puts(ptr @.str.0)
  ret i32 0
}
