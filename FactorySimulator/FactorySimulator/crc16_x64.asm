.code

; Внешняя ассемблерная функция для такта сдвига битов CRC16
; Параметры x64: CX (младшие 16 бит от RCX) = текущее значение CRC
; Возвращает результат в регистре AX

process_crc_bits proc
    mov ax, cx              ; Копируем входной CRC в AX
    mov r8d, 8              ; Счетчик цикла на 8 бит

bit_loop:
    test ax, 1              ; Проверяем младший значащий бит
    jz shift_only           ; Если бит равен 0, просто сдвигаем
    shr ax, 1               ; Сдвигаем CRC вправо на 1 бит
    xor ax, 0A001h          ; Делаем XOR с полиномом Modbus
    jmp loop_end
    
shift_only:
    shr ax, 1               ; Просто сдвигаем CRC вправо
    
loop_end:
    dec r8d                 ; Уменьшаем счетчик бит
    jnz bit_loop            ; Повторяем для всех 8 бит

    ret                     ; Возвращаем итоговый 16-битный CRC в AX
process_crc_bits endp

end
