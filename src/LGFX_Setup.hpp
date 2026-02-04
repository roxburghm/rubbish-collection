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

            // SPI Configuration
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0; // SPI Mode 0 or 3
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.spi_3wire  = true; // ST7789 usually 3-wire SPI (MOSI used for MOSI+MISO via sharing or just MOSI)
            cfg.use_lock   = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            
            // User Provided Pins:
            // SCK  = 12
            // DC   = 11
            // CS   = 10
            // MISO = 13 (Likely used as MOSI/SDA for display write)
            
            cfg.pin_sclk = 12;
            cfg.pin_mosi = 13; // Using 13 as Data In
            cfg.pin_miso = -1; // Not using MISO for reading currently
            cfg.pin_dc   = 11;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();

            cfg.pin_cs           = 10;
            cfg.pin_rst          = 1;
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
            cfg.pin_bl      = 14; 
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};
