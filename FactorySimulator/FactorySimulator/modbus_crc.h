#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    uint16_t calculateCRC16_Asm(const uint8_t* data, uint32_t length);

#ifdef __cplusplus
}
#endif
