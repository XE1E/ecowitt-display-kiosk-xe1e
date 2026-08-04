/*****************************************************************************
 * | File         :   io_extension.c
 * | Author       :   Waveshare team
 * | Function     :   IO_EXTENSION GPIO control via I2C interface
 * | Info         :
 * |                 I2C driver code for controlling GPIO pins using IO_EXTENSION chip.
 * ----------------
 * | This version :   V1.0
 * | Date         :   2024-11-27
 * | Info         :   Basic version, includes functions to read and write 
 * |                 GPIO pins using I2C communication with IO_EXTENSION.
 *
 ******************************************************************************/
#include "io_extension.h"  // Include IO_EXTENSION driver header for GPIO functions

// De este driver solo se usa lo que necesita esta placa: poner pines en salida y
// escribirlos (reset del GT911 en IO1, backlight en IO2). Las funciones de entrada,
// PWM y ADC del original se quitaron: no se usan, y la de PWM ademas confundia
// (el backlight de esta placa cuelga de un expansor DIGITAL, sin PWM; el brillo se
// hace escalando los pixeles en main.cpp).

io_extension_obj_t IO_EXTENSION;  // Define the global IO_EXTENSION object

/**
 * @brief Set the IO mode for the specified pins.
 * 
 * This function sets the specified pins to input or output mode by writing to the mode register.
 * 
 * @param pin An 8-bit value where each bit represents a pin (0 = input, 1 = output).
 */
void IO_EXTENSION_IO_Mode(uint8_t pin) 
{
    uint8_t data[2] = {IO_EXTENSION_Mode, pin}; // Prepare the data to write to the mode register
    // Write the 8-bit value to the IO mode register
    DEV_I2C_Write_Nbyte(IO_EXTENSION.addr, data, 2);
}

/**
 * @brief Initialize the IO_EXTENSION device.
 * 
 * This function configures the slave addresses for different registers of the
 * IO_EXTENSION chip via I2C, and sets the control flags for input/output modes.
 */
void IO_EXTENSION_Init()
{
    // Set the I2C slave address for the IO_EXTENSION device
    DEV_I2C_Set_Slave_Addr(&IO_EXTENSION.addr, IO_EXTENSION_ADDR);

    IO_EXTENSION_IO_Mode(0xff); // Set all pins to output mode

    // Initialize control flags for IO output enable and open-drain output mode
    IO_EXTENSION.Last_io_value = 0xFF; // All pins are initially set to high (output mode)
    IO_EXTENSION.Last_od_value = 0xFF; // All pins are initially set to high (open-drain mode)
}

/**
 * @brief Set the value of the IO output pins on the IO_EXTENSION device.
 * 
 * This function writes an 8-bit value to the IO output register. The value
 * determines the high or low state of the pins.
 * 
 * @param pin The pin number to set (0-7).
 * @param value The value to set on the specified pin (0 = low, 1 = high).
 */
void IO_EXTENSION_Output(uint8_t pin, uint8_t value) 
{
    // Update the output value based on the pin and value
    if (value == 1)
        IO_EXTENSION.Last_io_value |= (1 << pin); // Set the pin high
    else
        IO_EXTENSION.Last_io_value &= (~(1 << pin)); // Set the pin low

    uint8_t data[2] = {IO_EXTENSION_IO_OUTPUT_ADDR, IO_EXTENSION.Last_io_value}; // Prepare the data to write to the output register
    // Write the 8-bit value to the IO output register
    DEV_I2C_Write_Nbyte(IO_EXTENSION.addr, data, 2);
}

