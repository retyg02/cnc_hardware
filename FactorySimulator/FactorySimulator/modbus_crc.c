#include <stdint.h>


extern uint16_t process_crc_bits(uint16_t crc);

uint16_t calculateCRC16_Asm(const uint8_t* data, uint32_t length) {
    uint16_t crc = 0xFFFF;
    uint32_t i;

    for (i = 0; i < length; ++i) {
        crc ^= data[i];

        crc = process_crc_bits(crc);
    }
    return crc;
}
