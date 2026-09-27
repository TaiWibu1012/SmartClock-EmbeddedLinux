/**
 * @file ssd1306_oled.c
 * @brief SSD1306 I2C OLED Driver and Graphics Utilities
 * @author PHUC TAI
 *
 * Thread Safety: All public ssd1306_* functions are protected by s_oled_mutex.
 * Currently only clock_thread calls render functions (Master Display Arbitrator
 * pattern in clock_screen.c), but the mutex provides defense-in-depth.
 */

#include "ssd1306_oled.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

static int s_i2c_fd = -1;
static uint8_t s_oled_buffer[SSD1306_BUFSIZE];

/* 
 * Thread Safety Guarantee:
 * All OLED buffer operations and I2C transactions are strictly guarded by s_oled_mutex.
 * The mutex is initialized as PTHREAD_MUTEX_RECURSIVE to allow safe nested drawing calls.
 * Furthermore, in application architecture, only clock_thread performs rendering
 * (Master Display Arbitrator pattern), ensuring zero framebuffer garbling.
 */
static pthread_mutex_t s_oled_mutex;
static bool s_mutex_initialized = false;

static void ensure_mutex_initialized(void)
{
    if (!s_mutex_initialized) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&s_oled_mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        s_mutex_initialized = true;
    }
}

/* Full 5x7 standard ASCII table */
static const uint8_t FONT_5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* Space */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}  /* z */
};

static void ssd1306_send_command(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd};
    if (write(s_i2c_fd, buf, 2) < 0) {
        perror("ssd1306: command write failed");
    }
}

int ssd1306_init(const char *i2c_dev_path, uint8_t i2c_addr)
{
    ensure_mutex_initialized();

    s_i2c_fd = open(i2c_dev_path, O_RDWR);
    if (s_i2c_fd < 0) {
        perror("ssd1306: Failed to open I2C device");
        return -1;
    }

    if (ioctl(s_i2c_fd, I2C_SLAVE, i2c_addr) < 0) {
        perror("ssd1306: Failed to acquire bus access / talk to slave");
        close(s_i2c_fd);
        s_i2c_fd = -1;
        return -1;
    }

    /* Hardware Init Sequence (SH1106 & SSD1306 compatible) */
    ssd1306_send_command(0xAE); /* Display OFF */
    ssd1306_send_command(0xD5); /* Set Display Clock Divide Ratio */
    ssd1306_send_command(0x80);
    ssd1306_send_command(0xA8); /* Set Multiplex Ratio */
    ssd1306_send_command(0x3F); /* 1/64 duty */
    ssd1306_send_command(0xD3); /* Set Display Offset */
    ssd1306_send_command(0x00);
    ssd1306_send_command(0x40); /* Set Start Line 0 */
    ssd1306_send_command(0x8D); /* Charge Pump Setting */
    ssd1306_send_command(0x14); /* Enable Charge Pump */
    ssd1306_send_command(0xA1); /* Segment Re-map */
    ssd1306_send_command(0xC8); /* COM Output Scan Direction */
    ssd1306_send_command(0xDA); /* COM Pins */
    ssd1306_send_command(0x12);
    ssd1306_send_command(0x81); /* Contrast */
    ssd1306_send_command(0xCF);
    ssd1306_send_command(0xD9); /* Pre-charge Period */
    ssd1306_send_command(0xF1);
    ssd1306_send_command(0xDB); /* VCOM Deselect Level */
    ssd1306_send_command(0x40);
    ssd1306_send_command(0xA4); /* Output RAM content */
    ssd1306_send_command(0xA6); /* Normal Display */
    ssd1306_send_command(0xAF); /* Display ON */

    ssd1306_clear();
    ssd1306_update();
    return 0;
}

void ssd1306_close(void)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    if (s_i2c_fd >= 0) {
        memset(s_oled_buffer, 0x00, sizeof(s_oled_buffer));
        ssd1306_send_command(0xAE);
        close(s_i2c_fd);
        s_i2c_fd = -1;
    }
    pthread_mutex_unlock(&s_oled_mutex);
}

void ssd1306_clear(void)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    memset(s_oled_buffer, 0x00, sizeof(s_oled_buffer));
    pthread_mutex_unlock(&s_oled_mutex);
}

void ssd1306_update(void)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    if (s_i2c_fd < 0) {
        pthread_mutex_unlock(&s_oled_mutex);
        return;
    }

    uint8_t tx_buf[SSD1306_WIDTH + 1];
    tx_buf[0] = 0x40; /* Data mode */

    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        ssd1306_send_command(0xB0 + page); /* Page address */
        ssd1306_send_command(0x02);        /* Lower column start (SH1106 offset 2px) */
        ssd1306_send_command(0x10);        /* Higher column start */

        memcpy(&tx_buf[1], &s_oled_buffer[page * SSD1306_WIDTH], SSD1306_WIDTH);
        if (write(s_i2c_fd, tx_buf, sizeof(tx_buf)) < 0) {
            perror("ssd1306: page write failed");
        }
    }
    pthread_mutex_unlock(&s_oled_mutex);
}

static inline void ssd1306_draw_pixel_unlocked(int x, int y, uint8_t color)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) return;

    if (color) {
        s_oled_buffer[x + (y / 8) * SSD1306_WIDTH] |= (1 << (y % 8));
    } else {
        s_oled_buffer[x + (y / 8) * SSD1306_WIDTH] &= ~(1 << (y % 8));
    }
}

void ssd1306_draw_pixel(int x, int y, uint8_t color)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    ssd1306_draw_pixel_unlocked(x, y, color);
    pthread_mutex_unlock(&s_oled_mutex);
}

static void ssd1306_draw_char_unlocked(int x, int y, char c, uint8_t size)
{
    if (c < 32 || c > 122) c = ' ';
    const uint8_t *char_map = FONT_5X7[c - 32];

    for (int i = 0; i < 5; i++) {
        uint8_t line = char_map[i];
        for (int j = 0; j < 8; j++) {
            if (line & 0x01) {
                if (size == 1) {
                    ssd1306_draw_pixel_unlocked(x + i, y + j, 1);
                } else {
                    for (int sx = 0; sx < size; sx++) {
                        for (int sy = 0; sy < size; sy++) {
                            ssd1306_draw_pixel_unlocked(x + (i * size) + sx, y + (j * size) + sy, 1);
                        }
                    }
                }
            }
            line >>= 1;
        }
    }
}

void ssd1306_draw_char(int x, int y, char c, uint8_t size)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    ssd1306_draw_char_unlocked(x, y, c, size);
    pthread_mutex_unlock(&s_oled_mutex);
}

void ssd1306_draw_string(int x, int y, const char *str, uint8_t size)
{
    if (!str) return;
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    int cursor_x = x;
    while (*str) {
        ssd1306_draw_char_unlocked(cursor_x, y, *str, size);
        cursor_x += (5 * size) + size;
        str++;
    }
    pthread_mutex_unlock(&s_oled_mutex);
}

void ssd1306_draw_hline(int x, int y, int length, uint8_t color)
{
    ensure_mutex_initialized();
    pthread_mutex_lock(&s_oled_mutex);
    for (int i = 0; i < length; i++) {
        ssd1306_draw_pixel_unlocked(x + i, y, color);
    }
    pthread_mutex_unlock(&s_oled_mutex);
}