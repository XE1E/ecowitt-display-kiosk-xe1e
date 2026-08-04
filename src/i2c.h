/**
 * i2c.h - Wrapper I2C sobre Wire para el driver del CH422G (io_extension).
 *
 * Adaptacion del driver Waveshare, reducida a lo que se usa: fijar la direccion
 * del esclavo y escribirle N bytes. El bus lo abre main.cpp con
 * Wire.begin(I2C_SDA, I2C_SCL) en los pines 8/9 (ver config.h); aqui no se
 * inicializa nada ni se definen pines, para que no haya dos sitios que digan
 * cuales son.
 *
 * El acceso al bus se serializa con el mutex g_i2c de main.cpp (lo comparten el
 * touch en el core 1 y el BME280 en el core 0).
 */

#ifndef __I2C_H
#define __I2C_H

#include <Arduino.h>
#include <Wire.h>

// Tipo para handle de dispositivo I2C (simplificado para Arduino: la direccion)
typedef uint8_t i2c_master_dev_handle_t;

inline void DEV_I2C_Set_Slave_Addr(i2c_master_dev_handle_t* handle, uint8_t addr) {
    *handle = addr;
}

inline esp_err_t DEV_I2C_Write_Nbyte(i2c_master_dev_handle_t addr, const uint8_t* data, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(data, len);
    return Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

#endif // __I2C_H
