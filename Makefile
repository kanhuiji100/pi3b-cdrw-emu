CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -D_GNU_SOURCE -DCDRW_HOST_TEST -DCDRW_DEBUG_LOG=1 \
           -DCDRW_INQUIRY_VENDOR='"OrangePi"' \
           -DCDRW_INQUIRY_PRODUCT='"DVD/BD USB 2.0"' \
           -DCDRW_INQUIRY_T10ID='"ORANGEPI_DVDBD_01"'
CPPFLAGS += -Isrc/core -Isrc/posix -Isrc/usb -Isrc/ui
LDLIBS  += -pthread

CORE_SRC = src/core/scsi.c src/core/media.c src/core/util.c \
           src/core/media_lock.c \
           src/posix/ffshim.c src/posix/storage_posix.c
USB_SRC  = src/usb/bot.c src/usb/ffs_gadget.c
UI_SRC   = src/ui/oled_linux.c src/ui/encoder_linux.c src/ui/ui.c \
           src/ui/usb_status.c src/ui/font5x7.c
DAEMON_SRC = src/main.c $(USB_SRC) $(UI_SRC) $(CORE_SRC)

BUILD = build

.PHONY: all clean test daemon

all: $(BUILD)/pi3b-cdrw-emu

daemon: $(BUILD)/pi3b-cdrw-emu

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/pi3b-cdrw-emu: $(DAEMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(DAEMON_SRC) $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_scsi: tests/test_scsi.c src/core/scsi.c src/core/util.c src/core/media_lock.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_media: tests/test_media.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(BUILD)/test_bot: tests/test_bot.c $(USB_SRC) $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

test: $(BUILD)/test_scsi $(BUILD)/test_media $(BUILD)/test_bot
	./$(BUILD)/test_scsi
	./$(BUILD)/test_media
	./$(BUILD)/test_bot

clean:
	rm -rf $(BUILD)
