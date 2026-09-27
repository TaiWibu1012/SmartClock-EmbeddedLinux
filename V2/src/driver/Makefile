# Khai báo các module cần tạo ra từ mã nguồn C
obj-m += btn_driver.o
obj-m += buzzer_driver.o

# Đường dẫn tới thư mục Kernel Source/Headers (Cross-compile từ Yocto hoặc trên Pi)
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean