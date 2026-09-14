CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic -Werror
CACHE ?= $(HOME)/.cache/linux-pci-driver-lab
KVER := 6.8.0-138-generic
KDIR ?= $(CACHE)/guest/usr/src/linux-headers-$(KVER)
.PHONY: all client driver bootstrap initramfs guest clean host-check
all: client driver
client: build/edu-client
build/edu-client: client/main.cpp include/edu_lab.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Iinclude $< -o $@ -pthread $(LDFLAGS)
build/edu-client-static: client/main.cpp include/edu_lab.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Iinclude $< -o $@ -pthread -static
driver:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/driver W=1 modules
bootstrap:
	CACHE="$(CACHE)" ./scripts/bootstrap.sh
initramfs: all
	CACHE="$(CACHE)" ./scripts/make-initramfs.sh
guest: initramfs
	CACHE="$(CACHE)" ./scripts/run-guest.sh
host-check: client
	./build/edu-client --help
clean:
	rm -rf build
	@if test -d "$(KDIR)"; then $(MAKE) -C "$(KDIR)" M=$(CURDIR)/driver clean; fi
