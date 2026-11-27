#pragma once
// LovyanGFX setup for the WT32-SC01 Plus (ST7796 over 8-bit parallel, FT6336 touch).
// Pin numbers are the commonly published ones for this board; NOT verified on hardware here.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

 public:
  LGFX() {
    {
      auto c = _bus.config();
      c.freq_write = 40000000;
      c.pin_wr = 47;
      c.pin_rd = -1;
      c.pin_rs = 0;
      const int d[8] = {9, 46, 3, 8, 18, 17, 16, 15};
      c.pin_d0 = d[0]; c.pin_d1 = d[1]; c.pin_d2 = d[2]; c.pin_d3 = d[3];
      c.pin_d4 = d[4]; c.pin_d5 = d[5]; c.pin_d6 = d[6]; c.pin_d7 = d[7];
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = -1;
      c.pin_rst = 4;
      c.pin_busy = -1;
      c.memory_width = 320;
      c.memory_height = 480;
      c.panel_width = 320;
      c.panel_height = 480;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.dummy_read_pixel = 8;
      c.dummy_read_bits = 1;
      c.readable = true;
      c.invert = true;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = true;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 45;
      c.invert = false;
      c.freq = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 0;
      c.x_max = 319;
      c.y_min = 0;
      c.y_max = 479;
      c.pin_int = 7;
      c.bus_shared = true;
      c.offset_rotation = 0;
      c.i2c_port = 1;
      c.i2c_addr = 0x38;
      c.pin_sda = 6;
      c.pin_scl = 5;
      c.freq = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
