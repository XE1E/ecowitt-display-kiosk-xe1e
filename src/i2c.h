/**
 * i2c.h - I2C wrapper compatible con Arduino
 *
 * Adaptación del driver Waveshare para usar Wire.h
 */

#ifndef __I2C_H
#define __I2C_H

#include <Arduino.h>
#include <Wire.h>

#define EXAMPLE_I2C_MASTER_SDA 19
#define EXAMPLE_I2C_MASTER_SCL 20
#define EXAMPLE_I2C_MASTER_FREQUENCY 400000

// Tipo para handle de dispositivo I2C (simplificado para Arduino)
typedef uint8_t i2c_master_dev_handle_t;

// Puerto I2C (dummy para compatibilidad)
typedef struct {
    TwoWire* wire;
} DEV_I2C_Port;

static TwoWire* _i2c_wire = &Wire;

inline DEV_I2C_Port DEV_I2C_Init() {
    DEV_I2C_Port port;
    port.wire = _i2c_wire;
    return port;
}

inline void DEV_I2C_Set_Slave_Addr(i2c_master_dev_handle_t* handle, uint8_t addr) {
    *handle = addr;
}

inline esp_err_t DEV_I2C_Write_Nbyte(i2c_master_dev_handle_t addr, const uint8_t* data, size_t len) {
    _i2c_wire->beginTransmission(addr);
    _i2c_wire->write(data, len);
    return _i2c_wire->endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

inline esp_err_t DEV_I2C_Read_Nbyte(i2c_master_dev_handle_t addr, uint8_t reg, uint8_t* data, size_t len) {
    _i2c_wire->beginTransmission(addr);
    _i2c_wire->write(reg);
    if (_i2c_wire->endTransmission(false) != 0) return ESP_FAIL;

    _i2c_wire->requestFrom((uint8_t)addr, (size_t)len);
    for (size_t i = 0; i < len && _i2c_wire->available(); i++) {
        data[i] = _i2c_wire->read();
    }
    return ESP_OK;
}

inline uint16_t DEV_I2C_Read_Word(i2c_master_dev_handle_t addr, uint8_t reg) {
    uint8_t data[2] = {0, 0};
    DEV_I2C_Read_Nbyte(addr, reg, data, 2);
    return (data[0] << 8) | data[1];
}

inline esp_err_t DEV_I2C_Write(DEV_I2C_Port* port, uint8_t addr, const uint8_t* data, size_t len) {
    return DEV_I2C_Write_Nbyte(addr, data, len);
}

inline esp_err_t DEV_I2C_Read(DEV_I2C_Port* port, uint8_t addr, uint8_t* data, size_t len) {
    _i2c_wire->requestFrom(addr, len);
    for (size_t i = 0; i < len && _i2c_wire->available(); i++) {
        data[i] = _i2c_wire->read();
    }
    return ESP_OK;
}

inline esp_err_t DEV_I2C_WriteReg(DEV_I2C_Port* port, uint8_t addr, uint16_t reg, const uint8_t* data, size_t len) {
    _i2c_wire->beginTransmission(addr);
    _i2c_wire->write((reg >> 8) & 0xFF);
    _i2c_wire->write(reg & 0xFF);
    if (data && len > 0) {
        _i2c_wire->write(data, len);
    }
    return _i2c_wire->endTransmission() == 0 ? ESP_OK : ESP_FAIL;
}

inline esp_err_t DEV_I2C_ReadReg(DEV_I2C_Port* port, uint8_t addr, uint16_t reg, uint8_t* data, size_t len) {
    _i2c_wire->beginTransmission(addr);
    _i2c_wire->write((reg >> 8) & 0xFF);
    _i2c_wire->write(reg & 0xFF);
    if (_i2c_wire->endTransmission(false) != 0) return ESP_FAIL;

    _i2c_wire->requestFrom(addr, len);
    for (size_t i = 0; i < len && _i2c_wire->available(); i++) {
        data[i] = _i2c_wire->read();
    }
    return ESP_OK;
}

#endif // __I2C_H
