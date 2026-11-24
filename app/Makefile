PROG1 = $(shell jq -r '.acapPackageConf.setup.appName' manifest.json)
OBJS1 = $(PROG1).c rtpcore.c mgrcore.c manager.c
PROGS = $(PROG1)
DEBUG_DIR = debug

PKGS = libpipewire-0.3 glib-2.0 gio-2.0 axevent axparameter jansson libcurl

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS)) -lm -pthread

CFLAGS += -Wall \
          -Wextra \
          -Wformat=2 \
          -Wpointer-arith \
          -Wstrict-prototypes \
          -Wmissing-prototypes \
          -Wdisabled-optimization \
          -Wfloat-equal \
          -W \
          -Werror \
          -pthread

all: $(PROGS)

$(PROG1): $(OBJS1)
	install -d $(DEBUG_DIR)
	$(CC) $^ $(CFLAGS) $(LIBS) $(LDFLAGS) -o $(DEBUG_DIR)/$@ $(LDLIBS)
	cp $(DEBUG_DIR)/$@ .
	$(STRIP) $@

clean:
	rm -rf $(PROGS) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp* $(DEBUG_DIR)
