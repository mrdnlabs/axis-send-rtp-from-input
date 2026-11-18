# Host unit tests for the pure parts of the ACAP (app/rtpcore.c).
#   make -C tests          build and run
#   make -C tests SAN=1    with AddressSanitizer and UndefinedBehaviorSanitizer

APP_DIR = ../app

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -I$(APP_DIR) \
          -Wall -Wextra -Wformat=2 -Wpointer-arith -Wstrict-prototypes \
          -Wmissing-prototypes -Wfloat-equal -Werror
LDLIBS += -lm

ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
LDFLAGS += -fsanitize=address,undefined
endif

.PHONY: test preview clean

test: test_rtpcore
	./test_rtpcore

test_rtpcore: test_rtpcore.c $(APP_DIR)/rtpcore.c $(APP_DIR)/rtpcore.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_rtpcore.c $(APP_DIR)/rtpcore.c $(LDLIBS)

# The page's preview must replay the app's gate exactly. Needs Node.js.
preview: gate_dump
	./gate_dump > gate_vectors.json
	node test_preview.js gate_vectors.json

gate_dump: gate_dump.c $(APP_DIR)/rtpcore.c $(APP_DIR)/rtpcore.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ gate_dump.c $(APP_DIR)/rtpcore.c $(LDLIBS)

clean:
	rm -f test_rtpcore gate_dump gate_vectors.json
