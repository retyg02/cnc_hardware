#include <stdint.h>

// Сообщаем Си-компилятору, что функция process_crc_bits написана отдельно на Ассемблере
extern uint16_t process_crc_bits(uint16_t crc);

// Наша главная Си-функция, которую вызывает C++ проект
uint16_t calculateCRC16_Asm(const uint8_t* data, uint32_t length) {
    uint16_t crc = 0xFFFF;
    uint32_t i;

    for (i = 0; i < length; ++i) {
        // Выполняем XOR младшего байта CRC с текущим байтом данных на Си
        crc ^= data[i];

        // И вызываем внешнюю АССЕМБЛЕРНУЮ функцию для супер-быстрого побитового сдвига!
        crc = process_crc_bits(crc);
    }
    return crc;
}
