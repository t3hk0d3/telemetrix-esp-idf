# Entry point for building and testing. `make help` lists the targets.
# The ESP-IDF environment is activated per command when idf.py is not on PATH already (IDF_EXPORT).

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.DEFAULT_GOAL := help

IDF_EXPORT ?= $(HOME)/esp/esp-idf-v6.1/export.sh

IDF := $(if $(shell command -v idf.py),,. "$(IDF_EXPORT)" >/dev/null &&)

.PHONY: help lint test-host fuzz host-server test-integration build-examples

help: ## List the targets
	@awk -F':.*## ' '/^[a-z0-9-]+:.*## / {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

lint: ## astyle and ruff through pre-commit
	uvx pre-commit run --all-files

test-host: ## Host unit tests on the ESP-IDF linux target
	$(IDF) tools/run_host_tests.sh

FUZZ_BUILD ?= test/fuzz/build
FUZZ_TARGETS ?= fuzz_frame fuzz_adapter

fuzz: ## Fuzz the frame assembler and the adapter (60 s per target)
	cmake -S test/fuzz -B $(FUZZ_BUILD) -DCMAKE_C_COMPILER=clang
	cmake --build $(FUZZ_BUILD)
	status=0; \
	for t in $(FUZZ_TARGETS); do \
		mkdir -p $(FUZZ_BUILD)/work/$$t test/fuzz/corpus/$$t; \
		$(FUZZ_BUILD)/$$t -max_total_time=60 $(FUZZ_BUILD)/work/$$t test/fuzz/corpus/$$t || status=1; \
	done; \
	exit $$status

host-server: ## Build the server for the linux target with the fake driver (host/server/build)
	$(IDF) tools/build_host_server.sh

VENV ?= tools/.venv

$(VENV)/.installed: tools/compat/requirements.txt
	if command -v uv >/dev/null; then \
		uv venv --allow-existing $(VENV) && uv pip install --python $(VENV)/bin/python -r $<; \
	else \
		python3 -m venv $(VENV) && $(VENV)/bin/pip install -r $<; \
	fi
	touch $@

test-integration: host-server $(VENV)/.installed ## pytest integration tests against the host server
	$(VENV)/bin/python -m pytest tools/tests

build-examples: ## Build every example for the targets it lists
	for ex in examples/*; do \
		for t in $$(cat $$ex/targets); do \
			$(IDF) idf.py -C $$ex -B $$ex/build-$$t -DIDF_TARGET=$$t \
				-DSDKCONFIG=$(CURDIR)/$$ex/build-$$t/sdkconfig build; \
		done; \
	done
