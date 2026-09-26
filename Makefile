.DEFAULT_GOAL := firmware
.NOTPARALLEL:
PYTHON ?= python

.PHONY: firmware q2 o8 q2-f4 check

firmware:
	"$(PYTHON)" tools/build_firmware.py

q2 o8 q2-f4:
	"$(PYTHON)" tools/build_firmware.py --variant $@

check:
	"$(PYTHON)" tools/generate_models.py --check
