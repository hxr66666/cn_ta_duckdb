PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=cn_ta
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override the upstream `test` target (which assumes a Docker-built unittest
# binary at build/release/test/unittest) with a local unittest build + run.
# Builds the unittest binary with the extension statically linked, then runs
# the sqllogic tests under test/sql/.
#
# `test_release_internal` is the actual command body of the upstream `test`
# chain (test -> test_release -> test_release_internal); redefining it here
# redirects the whole chain to our script without fighting Make's prerequisite
# merging on the `test` target itself.
.PHONY: test-sql
test-sql:
	$(PROJ_DIR)scripts/test_linux.sh

test_release_internal:
	$(PROJ_DIR)scripts/test_linux.sh
