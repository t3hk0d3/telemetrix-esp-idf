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

fuzz: ## Fuzz the frame assembler and the adapter (60 s per target)
	@echo "fuzz: not yet"

host-server: ## Run the server on the linux target with fake drivers
	@echo "host-server: not yet"

test-integration: ## pytest integration tests against the host server
	@echo "test-integration: not yet"

build-examples: ## Build every example for the targets it lists
	@echo "build-examples: not yet"
