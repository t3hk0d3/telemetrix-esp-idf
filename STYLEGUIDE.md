# Styleguide

Every change follows these rules.

- **C formatting:** astyle via pre-commit (`astyle_py`, rules in `astyle-rules.yml`: 4-space indent, attached braces,
  120 columns, pointer to name). `make lint` must pass and leave `git status` clean.
- **C naming:** `snake_case`. Public API prefix `telemetrix_` / `TELEMETRIX_` only in `include/`. Internal prefix
  `tmx_` / `TMX_`. One module per file; a module `foo` lives in `foo.c` with declarations in its layer's header
  (`src/proto/tmx_proto.h`, `src/core/tmx_core.h`, `src/adapter/tmx_adapter.h`, `src/drivers/tmx_drv.h`,
  `src/net/tmx_net.h`). Static functions have no prefix.
- **Layers and includes:** `proto` and `core` and `adapter` include only C standard headers, `esp_err.h` and
  `include/telemetrix.h` (they build for every target and for the fuzzer). Only `src/drivers/esp/` includes IDF driver
  headers. `src/net/` includes POSIX socket headers (`sys/socket.h`, `netinet/in.h`, `netinet/tcp.h`) that resolve to
  lwIP on targets and to the host on `linux`.
- **Errors:** functions that can fail return `esp_err_t`; `ESP_ERR_INVALID_ARG` for bad input, `ESP_ERR_INVALID_STATE`
  for wrong order, `ESP_ERR_NO_MEM`, `ESP_ERR_NOT_FOUND`, `ESP_ERR_TIMEOUT` from drivers for "no answer". Never
  `abort()`/`assert()` on input from the wire or the application. Every wire-caused refusal increments a counter.
- **Logging:** one `static const char *TAG = "telemetrix";` per file that logs; no logging on hot paths below `D`.
- **Banned:** `gpio_reset_pin` (enables the pull-up; spec 4.3); dynamic allocation after `telemetrix_start` returns
  except the send queue and driver handles created on a client's request; any IDF call with an infinite timeout.
- **Config:** compile-time switches in `Kconfig` as `CONFIG_TELEMETRIX_<FEATURE>`; runtime values in
  `telemetrix_config_t`; hardware facts in the profile, never in Kconfig.
- **Tests:** host unit tests in `test_host/main/test_<module>.c` using Unity,
  `TEST_CASE("<scenario>: <effect>", "[<module>]")`, one test per effect of the BDD skeleton in the plan. Python in `tools/`
  formatted and linted with ruff; pytest files `tools/tests/test_<topic>.py`, functions `test_<scenario>_<effect>`.
- **Commands:** every documented command is a `make` target with a `## ` help comment: `make lint`, `make test-host`,
  `make fuzz` (60 s per target), `make host-server`, `make test-integration`, `make build-examples`, `make help`.
  Run them as `<cmd> 2>&1 | tee $SCRATCH/<name>.log` (`set -o pipefail;` first when the exit code matters).
- **Commits:** Conventional Commits (`feat(core): …`, `fix(net): …`, `test: …`, `docs: …`, `chore: …`), each ending
  with the `Co-Authored-By` line the session gives.
