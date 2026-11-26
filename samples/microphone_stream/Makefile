PS5_HOST ?= ps5
PS5_PORT ?= 9021
CAPTURE_SECONDS ?= 30

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := microphone_stream.elf
CFLAGS := -Wall -Werror -I$(PS5_PAYLOAD_SDK)/target/include -DCAPTURE_SECONDS=$(CAPTURE_SECONDS)
LDFLAGS := -L$(PS5_PAYLOAD_SDK)/target/lib -lps5platform -lSceAudioIn -lSceUserService

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
