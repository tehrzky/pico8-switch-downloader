ifneq ($(BUILD),$(notdir $(CURDIR)))

export TOPDIR  := $(CURDIR)
export LIBNX   := $(DEVKITPRO)/libnx
export ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft

include $(DEVKITPRO)/devkita64/base_rules

export TARGET  := pico8-downloader
export BUILD   := build
export SOURCES := source
export INCLUDES:= include

export CFLAGS  := -O2 -Wall -ffunction-sections $(ARCH) -D__SWITCH__
export CXXFLAGS:= $(CFLAGS) -std=gnu++17
export LIBDIRS := $(PORTLIBS)/switch $(LIBNX)

export LIBS    := -lcurl -lmbedtls -lmbedcrypto -lmbedx509 -lz -lsdl2 -lnx

.PHONY: $(BUILD) clean

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

all: $(TOPDIR)/$(TARGET).nro

$(TOPDIR)/$(TARGET).nro: $(TOPDIR)/$(TARGET).elf
$(TOPDIR)/$(TARGET).elf: $(OFILES)

-include $(DEPENDS)

endif
