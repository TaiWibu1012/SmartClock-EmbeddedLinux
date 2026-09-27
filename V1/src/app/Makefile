# ==============================================================================
# Makefile for Smart Weather Alarm Clock (Userspace Application)
# Enterprise Embedded Linux Standard
# ==============================================================================

CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -O2 -pipe -I../../include -I.
LDFLAGS ?= -pthread -lrt -lm

# Target executable name
TARGET = smartclock

# Source files
SRCS = main.c \
       clock_screen.c \
       weather_screen.c \
       alarm_manager.c \
       smartconfig.c \
       webserver.c \
       ssd1306_oled.c

# Object files
OBJS = $(SRCS:.c=.o)

.PHONY: all clean debug

ifeq ($(DEBUG),1)
CFLAGS += -g3 -O0 -DDEBUG
endif

all: $(TARGET)

debug: CFLAGS += -g3 -O0 -DDEBUG
debug: clean $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "=================================================="
	@echo "  Built Userspace Application: $(TARGET) [OK]"
	@echo "=================================================="

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned Userspace build artifacts."