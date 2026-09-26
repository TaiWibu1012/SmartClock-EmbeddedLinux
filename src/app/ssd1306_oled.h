/**
 * @file ssd1306_oled.h
 * @brief SSD1306 I2C OLED Driver and Graphics Utilities
 * @author PHUC TAI
 */

#ifndef _SSD1306_OLED_H_
#define _SSD1306_OLED_H_

#include <stdint.h>
#include <stdbool.h>

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES   8
#define SSD1306_BUFSIZE (SSD1306_WIDTH * SSD1306_PAGES) /* 1024 bytes */

int  ssd1306_init(const char *i2c_dev_path, uint8_t i2c_addr);
void ssd1306_close(void);
void ssd1306_clear(void);
void ssd1306_update(void);
void ssd1306_draw_pixel(int x, int y, uint8_t color);
void ssd1306_draw_char(int x, int y, char c, uint8_t size);
void ssd1306_draw_string(int x, int y, const char *str, uint8_t size);
void ssd1306_draw_hline(int x, int y, int length, uint8_t color);

#endif /* _SSD1306_OLED_H_ */