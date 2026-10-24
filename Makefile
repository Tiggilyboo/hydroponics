# ESP-IDF 5.5.4 image: no host-side ESP-IDF installation required.
IMAGE ?= espressif/idf:v5.5.4
PORT ?= /dev/ttyACM0
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
HOST_UID := $(shell id -u)
HOST_GID := $(shell id -g)
PORT_GID := $(shell stat -c '%g' '$(PORT)' 2>/dev/null || echo 0)
DOCKER_BASE = docker run --rm --init --user $(HOST_UID):$(HOST_GID) \
  -e HOME=/tmp -e IDF_GIT_SAFE_DIR=/project \
  -v '$(ROOT):/project' -w /project
IDF = $(DOCKER_BASE) $(IMAGE) idf.py
IDF_SERIAL = $(DOCKER_BASE) --device '$(PORT):$(PORT)' \
  --group-add $(PORT_GID) $(IMAGE) idf.py -p '$(PORT)'

.PHONY: init build flash monitor menuconfig erase clean shell

init:
	$(IDF) set-target esp32c6

build:
	$(IDF) build

flash: build
	$(IDF_SERIAL) flash

monitor:
	$(DOCKER_BASE) -it --device '$(PORT):$(PORT)' --group-add $(PORT_GID) $(IMAGE) idf.py -p '$(PORT)' monitor

menuconfig:
	$(DOCKER_BASE) -it $(IMAGE) idf.py menuconfig

erase:
	@echo 'WARNING: This erases Zigbee pairing and all flash data.'
	$(IDF_SERIAL) erase-flash

clean:
	$(IDF) fullclean

shell:
	$(DOCKER_BASE) -it $(IMAGE) bash
