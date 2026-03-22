#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789      _panel_instance;
    lgfx::Bus_SPI           _bus_instance;
    lgfx::Light_PWM         _light_instance;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            // SPI Configuration for Waveshare ESP32-C6-Touch-LCD-1.9
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = true;
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;

            cfg.pin_sclk = 5;
            cfg.pin_mosi = 4;
            cfg.pin_miso = -1;
            cfg.pin_dc   = 6;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();

            cfg.pin_cs           = 7;
            cfg.pin_rst          = 14;
            cfg.pin_busy         = -1;

            cfg.panel_width      = 170;
            cfg.panel_height     = 320;
            cfg.offset_x         = 35;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = true;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;

            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = 15;
            cfg.invert      = true;   // Waveshare C6 board has inverted backlight polarity
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;      // ESP32-C6 has LEDC channels 0-5 only

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};
