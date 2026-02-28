# Makefile for iroh-libevent
#
# This Makefile builds the Rust library, the rampart-iroh module, and the C examples.

CARGO ?= cargo
CC ?= gcc
CFLAGS ?= -Wall -Wextra -g -O2
PKG_CONFIG ?= pkg-config

# Rampart include path (auto-detect via rampart, fallback to default)
RAMPART_INCLUDE ?= $(or $(shell rampart -c 'console.log(process.installPath)' 2>/dev/null | tr -d '\r'),/usr/local/rampart)/include

# Local libevent fallback path (used on both Linux and macOS if not found elsewhere)
LIBEVENT_LOCAL := /usr/local/src/rampart/extern/libevent

# Paths
TARGET_DIR := target/release
INCLUDE_DIR := include
LIB_NAME := iroh_libevent
MODULE_DIR := module

# Platform-specific library names and system libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	DYLIB_EXT := dylib
	STATIC_LIB := lib$(LIB_NAME).a
	DYNAMIC_LIB := lib$(LIB_NAME).$(DYLIB_EXT)
	# macOS requires linking against system frameworks used by Rust crates
	# (security-framework, core-foundation, SystemConfiguration, etc.)
	SYS_LIBS := -framework Security -framework CoreFoundation -framework SystemConfiguration \
	            -framework CoreServices -liconv -lresolv
	# Set deployment target for macOS 11.0 (Big Sur) compatibility
	export MACOSX_DEPLOYMENT_TARGET := 11.0
	LDFLAGS += -mmacosx-version-min=11.0
	# macOS shared library flags
	MODULE_LDFLAGS := -dynamiclib -undefined dynamic_lookup -install_name rampart-iroh.so

	# On macOS, first try pkg-config with brew prefix, then local fallback
	BREW_PREFIX := $(shell brew --prefix libevent 2>/dev/null)
	ifneq ($(BREW_PREFIX),)
		LIBEVENT_CFLAGS := $(shell PKG_CONFIG_PATH="$(BREW_PREFIX)/lib/pkgconfig" $(PKG_CONFIG) --cflags libevent 2>/dev/null)
		LIBEVENT_LIBS   := $(shell PKG_CONFIG_PATH="$(BREW_PREFIX)/lib/pkgconfig" $(PKG_CONFIG) --libs libevent 2>/dev/null)
	endif
else ifneq ($(filter MSYS_NT% MINGW64_NT% MINGW32_NT%,$(UNAME_S)),)
	# Use MSYS gcc (not MinGW gcc) for the module to match rampart's ABI.
	# MinGW gcc may be first on PATH (needed for cargo), but the module must
	# link against msys-2.0.dll like rampart does.
	CC := /c/tools/msys64/usr/bin/gcc.exe
	DYLIB_EXT := so
	STATIC_LIB := lib$(LIB_NAME).a
	DYNAMIC_LIB := lib$(LIB_NAME).$(DYLIB_EXT)
	# Windows/MSYS2: link against an MSYS-compatible import library for the MinGW-built DLL.
	# The MinGW .dll.a has PE relocator refs that MSYS ld can't resolve, so we regenerate
	# the import lib using MSYS dlltool with only the public C API symbols.
	MODULE_LINK_LIB := lib$(LIB_NAME)_msys.dll.a
	# Windows/MSYS2: link against rampart import library
	RAMPART_BIN := $(or $(shell rampart -c 'console.log(process.installPath)' 2>/dev/null | tr -d '\r'),/usr/local/rampart)/bin
	SYS_LIBS := '-L$(RAMPART_BIN)' -lrampart
	# MSYS2 shared library flags (disable pseudo-reloc: no data imports from the DLL)
	MODULE_LDFLAGS := -fPIC -shared -Wl,--disable-runtime-pseudo-reloc
else
	DYLIB_EXT := so
	STATIC_LIB := lib$(LIB_NAME).a
	DYNAMIC_LIB := lib$(LIB_NAME).$(DYLIB_EXT)
	# Linux system libraries
	SYS_LIBS := -lpthread -ldl -lm
	# Linux shared library flags
	MODULE_LDFLAGS := -fPIC -shared -Wl,-soname,rampart-iroh.so

	# On Linux, try pkg-config normally
	LIBEVENT_CFLAGS := $(shell $(PKG_CONFIG) --cflags libevent 2>/dev/null)
	LIBEVENT_LIBS   := $(shell $(PKG_CONFIG) --libs libevent 2>/dev/null)
endif

# Default: link module against the static library (overridden to DLL import lib on Windows)
MODULE_LINK_LIB ?= $(STATIC_LIB)

# If libevent was not found via pkg-config (or brew), fall back to the local extern path
ifeq ($(LIBEVENT_LIBS),)
	ifneq ($(wildcard $(LIBEVENT_LOCAL)/lib/libevent.a $(LIBEVENT_LOCAL)/lib/libevent.so \
	                  $(LIBEVENT_LOCAL)/lib/libevent.dylib),)
		LIBEVENT_CFLAGS := -I$(LIBEVENT_LOCAL)/include
		LIBEVENT_LIBS   := -L$(LIBEVENT_LOCAL)/lib -levent
	else
		# Last-resort: hope the linker finds it on its own
		LIBEVENT_LIBS := -levent
	endif
endif

# Test if libevent is actually usable (headers + library available)
HAVE_LIBEVENT := $(shell printf '\#include <event2/event.h>\nint main(){return 0;}' | \
    $(CC) -x c - -o /dev/null $(LIBEVENT_CFLAGS) $(LIBEVENT_LIBS) 2>/dev/null && echo yes)

# Examples (only built if libevent is available)
EXAMPLES := echo_server echo_client gossip_chat blob_transfer docs_kv docs_persistent blobs_persistent
ifneq ($(HAVE_LIBEVENT),yes)
    EXAMPLES :=
endif

# Module
MODULE := rampart-iroh.so

.PHONY: all lib module examples clean install

all: lib module examples

lib:
	$(CARGO) build --release
	@echo "Library built: $(TARGET_DIR)/$(STATIC_LIB)"
	@echo "Library built: $(TARGET_DIR)/$(DYNAMIC_LIB)"

module: lib $(MODULE)

# On Windows/MSYS2, generate an MSYS-compatible import library from the MinGW-built DLL
$(TARGET_DIR)/lib$(LIB_NAME)_msys.dll.a: $(TARGET_DIR)/$(LIB_NAME).dll
	@echo "Generating MSYS-compatible import library..."
	nm $< | grep " T iroh_" | awk '{print "    " $$3}' > $(TARGET_DIR)/$(LIB_NAME)_msys.def
	sed -i '1i LIBRARY $(LIB_NAME).dll\nEXPORTS' $(TARGET_DIR)/$(LIB_NAME)_msys.def
	dlltool -d $(TARGET_DIR)/$(LIB_NAME)_msys.def -l $@ -D $(LIB_NAME).dll

$(MODULE): $(MODULE_DIR)/rampart-iroh.c $(TARGET_DIR)/$(MODULE_LINK_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $(MODULE_LDFLAGS) \
		-I$(INCLUDE_DIR) '-I$(RAMPART_INCLUDE)' $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(MODULE_LINK_LIB) \
		$(SYS_LIBS)

examples: lib $(EXAMPLES)
ifneq ($(HAVE_LIBEVENT),yes)
	@echo "Note: libevent2 not found — skipping example C programs"
endif

# Link with static library - order matters: object files, then static lib, then system libs
echo_server: examples/echo_server.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

echo_client: examples/echo_client.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

gossip_chat: examples/gossip_chat.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

blob_transfer: examples/blob_transfer.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

docs_kv: examples/docs_kv.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

docs_persistent: examples/docs_persistent.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

blobs_persistent: examples/blobs_persistent.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(INCLUDE_DIR) $(LIBEVENT_CFLAGS) \
		-o $@ $< \
		$(TARGET_DIR)/$(STATIC_LIB) \
		$(LIBEVENT_LIBS) $(SYS_LIBS)

clean:
	$(CARGO) clean
	rm -f $(EXAMPLES) $(MODULE)
	rm -rf $(addsuffix .dSYM,$(EXAMPLES)) $(MODULE).dSYM

# Install rampart module
RAMPART_INSTALL := $(shell rampart -c 'console.log(process.installPath)' 2>/dev/null || echo /usr/local/rampart)
RAMPART_MODULES := $(RAMPART_INSTALL)/modules
RAMPART_TEST := $(RAMPART_INSTALL)/test

install: module
	install -d '$(RAMPART_MODULES)'
	install -m 755 $(MODULE) '$(RAMPART_MODULES)/'
ifneq ($(filter MSYS_NT% MINGW64_NT% MINGW32_NT%,$(UNAME_S)),)
	install -m 755 $(TARGET_DIR)/$(LIB_NAME).dll '$(RAMPART_MODULES)/'
else
	strip -S '$(RAMPART_MODULES)/$(MODULE)'
endif
	@if [ -d '$(RAMPART_TEST)' ]; then \
		install -m 644 iroh-test.js '$(RAMPART_TEST)/'; \
		echo "Installed iroh-test.js to $(RAMPART_TEST)/"; \
	fi
	@if [ -d '$(RAMPART_INSTALL)/licenses' ]; then \
		cp licenses/* '$(RAMPART_INSTALL)/licenses/'; \
		echo "Installed license files to $(RAMPART_INSTALL)/licenses/"; \
	fi

# Generate header file only
header:
	$(CARGO) build --release
	@echo "Header generated: $(INCLUDE_DIR)/iroh_libevent.h"

# Run the examples (static linking means no LD_LIBRARY_PATH needed)
run-server: echo_server
	./echo_server

run-client: echo_client
	@if [ -z "$(ADDR)" ]; then \
		echo "Usage: make run-client ADDR=<endpoint_address>"; \
		exit 1; \
	fi
	./echo_client "$(ADDR)"

run-gossip-create: gossip_chat
	./gossip_chat create

run-gossip-join: gossip_chat
	@if [ -z "$(TOPIC)" ] || [ -z "$(PEER)" ]; then \
		echo "Usage: make run-gossip-join TOPIC=<topic_id> PEER=<peer_id>"; \
		exit 1; \
	fi
	./gossip_chat join "$(TOPIC)" "$(PEER)"

run-blob-send: blob_transfer
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make run-blob-send FILE=<file_path>"; \
		exit 1; \
	fi
	./blob_transfer send "$(FILE)"

run-blob-receive: blob_transfer
	@if [ -z "$(TICKET)" ]; then \
		echo "Usage: make run-blob-receive TICKET=<blob_ticket>"; \
		exit 1; \
	fi
	./blob_transfer receive "$(TICKET)"

run-docs-create: docs_kv
	./docs_kv create

run-docs-join: docs_kv
	@if [ -z "$(TICKET)" ]; then \
		echo "Usage: make run-docs-join TICKET=<doc_ticket>"; \
		exit 1; \
	fi
	./docs_kv join "$(TICKET)"

run-docs-set: docs_kv
	@if [ -z "$(TICKET)" ] || [ -z "$(KEY)" ] || [ -z "$(VALUE)" ]; then \
		echo "Usage: make run-docs-set TICKET=<doc_ticket> KEY=<key> VALUE=<value>"; \
		exit 1; \
	fi
	./docs_kv set "$(TICKET)" "$(KEY)" "$(VALUE)"

run-docs-get: docs_kv
	@if [ -z "$(TICKET)" ] || [ -z "$(KEY)" ]; then \
		echo "Usage: make run-docs-get TICKET=<doc_ticket> KEY=<key>"; \
		exit 1; \
	fi
	./docs_kv get "$(TICKET)" "$(KEY)"

# Persistent storage examples
run-docs-persistent-create: docs_persistent
	./docs_persistent create

run-docs-persistent-join: docs_persistent
	@if [ -z "$(TICKET)" ]; then \
		echo "Usage: make run-docs-persistent-join TICKET=<doc_ticket>"; \
		echo "       IROH_STORAGE=/path/to/storage make run-docs-persistent-join TICKET=<doc_ticket>"; \
		exit 1; \
	fi
	./docs_persistent join "$(TICKET)"

run-docs-persistent-set: docs_persistent
	@if [ -z "$(TICKET)" ] || [ -z "$(KEY)" ] || [ -z "$(VALUE)" ]; then \
		echo "Usage: make run-docs-persistent-set TICKET=<doc_ticket> KEY=<key> VALUE=<value>"; \
		exit 1; \
	fi
	./docs_persistent set "$(TICKET)" "$(KEY)" "$(VALUE)"

run-docs-persistent-get: docs_persistent
	@if [ -z "$(TICKET)" ] || [ -z "$(KEY)" ]; then \
		echo "Usage: make run-docs-persistent-get TICKET=<doc_ticket> KEY=<key>"; \
		exit 1; \
	fi
	./docs_persistent get "$(TICKET)" "$(KEY)"

run-blobs-persistent-serve: blobs_persistent
	./blobs_persistent serve

run-blobs-persistent-add: blobs_persistent
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make run-blobs-persistent-add FILE=<file_path>"; \
		exit 1; \
	fi
	./blobs_persistent add "$(FILE)"

run-blobs-persistent-get: blobs_persistent
	@if [ -z "$(TICKET)" ]; then \
		echo "Usage: make run-blobs-persistent-get TICKET=<blob_ticket>"; \
		exit 1; \
	fi
	./blobs_persistent get "$(TICKET)"

# Help target
help:
	@echo "iroh-libevent Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build library and examples (default)"
	@echo "  lib              - Build the Rust library only"
	@echo "  examples         - Build the C examples"
	@echo "  header           - Generate the C header file"
	@echo "  clean            - Remove build artifacts"
	@echo "  install          - Install rampart-iroh.so to rampart modules directory"
	@echo "  run-server       - Run the echo server example"
	@echo "  run-client       - Run the echo client example (requires ADDR=...)"
	@echo "  run-gossip-create - Create a new gossip topic"
	@echo "  run-gossip-join  - Join a gossip topic (requires TOPIC=... PEER=...)"
	@echo "  run-blob-send    - Share a file (requires FILE=...)"
	@echo "  run-blob-receive - Download a file (requires TICKET=...)"
	@echo "  run-docs-create  - Create a new synced document"
	@echo "  run-docs-join    - Join a document (requires TICKET=...)"
	@echo "  run-docs-set     - Set a key-value (requires TICKET=... KEY=... VALUE=...)"
	@echo "  run-docs-get     - Get a value (requires TICKET=... KEY=...)"
	@echo ""
	@echo "Persistent Storage Examples:"
	@echo "  run-docs-persistent-create  - Create doc with persistent storage"
	@echo "  run-docs-persistent-join    - Join doc with persistent storage"
	@echo "  run-docs-persistent-set     - Set value with persistent storage"
	@echo "  run-docs-persistent-get     - Get value with persistent storage"
	@echo "  run-blobs-persistent-serve  - Serve blobs from persistent storage"
	@echo "  run-blobs-persistent-add    - Add file to persistent blob storage"
	@echo "  run-blobs-persistent-get    - Download blob to persistent storage"
	@echo ""
	@echo "Variables:"
	@echo "  RAMPART_MODULES  - Override rampart modules directory"
	@echo "  CC               - C compiler (default: gcc)"
	@echo "  CFLAGS           - C compiler flags"
	@echo "  IROH_STORAGE     - Path to persistent storage (for persistent examples)"
	@echo ""
	@echo "Examples:"
	@echo "  make"
	@echo "  make install"
	@echo "  make run-server"
	@echo "  make run-client ADDR=\"abc123...\""
	@echo "  make run-gossip-create"
	@echo "  make run-gossip-join TOPIC=\"xyz...\" PEER=\"abc...\""
	@echo "  make run-blob-send FILE=\"myfile.txt\""
	@echo "  make run-blob-receive TICKET=\"blob....\""
	@echo "  make run-docs-create"
	@echo "  make run-docs-set TICKET=\"doc....\" KEY=\"mykey\" VALUE=\"myvalue\""
	@echo "  make run-docs-get TICKET=\"doc....\" KEY=\"mykey\""
	@echo ""
	@echo "Persistent Storage Examples:"
	@echo "  # Create a document with persistent storage:"
	@echo "  make run-docs-persistent-create"
	@echo "  # Note the storage path and ticket printed"
	@echo ""
	@echo "  # Rejoin after restart (using same storage):"
	@echo "  IROH_STORAGE=/tmp/iroh-docs-12345 make run-docs-persistent-join TICKET=\"...\""
	@echo ""
	@echo "  # Add file to persistent blob storage:"
	@echo "  make run-blobs-persistent-add FILE=\"myfile.txt\""
	@echo "  # Blob persists after restart!"
