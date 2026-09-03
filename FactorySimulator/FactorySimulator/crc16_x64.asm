mov cl, 8
bit_loop:
test ax, 1
jz shift_only
shr ax, 1
xor ax, 0A001h
jmp loop_end
shift_only:
shr ax, 1
loop_end:
dec cl
jnz bit_loop
ret