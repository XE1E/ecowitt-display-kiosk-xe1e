/**
 * bme280_sensor.h - BME280 Indoor Sensor
 *
 * Sensor local de temperatura, humedad y presión.
 * Puede enviar datos al servidor como estación remota.
 */

#ifndef BME280_SENSOR_H
#define BME280_SENSOR_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "config.h"

// BME280 instance
static Adafruit_BME280 bme;
static bool bmeInitialized = false;
static float bmePressureOffset = 0;     // Offset de calibracion en hPa
static float bmeTemperatureOffset = 0;  // Offset de calibracion en grados C
static float bmeHumidityOffset = 0;     // Offset de calibracion en %

// Setters para los offsets (llamar desde main al arrancar)
void setBME280PressureOffset(float offset) {
    bmePressureOffset = offset;
}

void setBME280TemperatureOffset(float offset) {
    bmeTemperatureOffset = offset;
}

void setBME280HumidityOffset(float offset) {
    bmeHumidityOffset = offset;
}

// ============================================================================
// Initialize BME280
// ============================================================================

bool initBME280(uint8_t address = 0x76) {
    Serial.println("[BME280] Inicializando...");
    Serial.printf("[BME280] Intentando direccion 0x%02X\n", address);

    // Pasar Wire explicitamente (I2C en GPIO 8/9)
    if (!bme.begin(address, &Wire)) {
        Serial.println("[BME280] No encontrado en direccion primaria");
        // Intentar direccion alternativa
        uint8_t altAddr = (address == 0x76) ? 0x77 : 0x76;
        Serial.printf("[BME280] Intentando direccion alternativa 0x%02X\n", altAddr);
        if (!bme.begin(altAddr, &Wire)) {
            Serial.println("[BME280] No encontrado!");
            bmeInitialized = false;
            return false;
        }
    }

    // Configurar para monitoreo interior
    bme.setSampling(
        Adafruit_BME280::MODE_NORMAL,
        Adafruit_BME280::SAMPLING_X2,   // temperature
        Adafruit_BME280::SAMPLING_X16,  // pressure
        Adafruit_BME280::SAMPLING_X1,   // humidity
        Adafruit_BME280::FILTER_X16,
        Adafruit_BME280::STANDBY_MS_500
    );

    bmeInitialized = true;
    Serial.println("[BME280] Inicializado OK");
    return true;
}

// ============================================================================
// Read Sensor Data
// ============================================================================

bool readBME280(LocalSensorData& data) {
    if (!bmeInitialized) {
        data.valid = false;
        return false;
    }

    data.temperature = bme.readTemperature() + bmeTemperatureOffset;
    data.humidity = bme.readHumidity() + bmeHumidityOffset;

    // Leer presion absoluta y convertir a presion relativa (nivel del mar)
    float pressure_abs = bme.readPressure() / 100.0F;  // hPa

#ifdef BME280_ALTITUDE
    // Formula barometrica con compensacion de temperatura
    // P0 = P * (1 + 0.0065*h / (T + 0.0065*h + 273.15))^5.257
    float h = (float)BME280_ALTITUDE;
    float T = data.temperature;  // Usar temperatura medida
    float exponent = (9.80665f * 0.0289644f) / (8.31447f * 0.0065f);  // ~5.257
    data.pressure = pressure_abs * pow(1.0f + (0.0065f * h) / (T + 273.15f), exponent);
#else
    data.pressure = pressure_abs;
#endif

    // Aplicar offset de calibracion
    data.pressure += bmePressureOffset;

    data.valid = true;
    data.last_read = millis();

    return true;
}

// ============================================================================
// Get Sensor Status
// ============================================================================

bool isBME280Available() {
    return bmeInitialized;
}

// ============================================================================
// Get formatted string
// ============================================================================

void getBME280String(char* buffer, size_t size, const LocalSensorData& data) {
    if (data.valid) {
        snprintf(buffer, size, "%.1f°C  %.0f%%  %.0fhPa",
                 data.temperature, data.humidity, data.pressure);
    } else {
        snprintf(buffer, size, "-- (sensor no disponible)");
    }
}

#endif // BME280_SENSOR_H
