# Copyright (c) 2026 Halo-Atlas Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

get_filename_component(HALO_ATLAS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES "${HALO_ATLAS_ROOT}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pebble_compat_qemu_gate)

set(EMBEDDED_PBW_HDR "${CMAKE_CURRENT_BINARY_DIR}/gen/pebble_embedded_pbw.h")
execute_process(
	COMMAND "${PYTHON_EXECUTABLE}"
		"${HALO_ATLAS_ROOT}/tools/gen_embedded_pbw.py"
		--pbw "${HALO_ATLAS_ROOT}/tests/fixtures/test-app.pbw"
		--out "${EMBEDDED_PBW_HDR}"
	RESULT_VARIABLE EMBED_RC
)
if(NOT EMBED_RC EQUAL 0)
	message(FATAL_ERROR "gen_embedded_pbw.py failed with exit code ${EMBED_RC}")
endif()

target_include_directories(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
target_sources(app PRIVATE src/main.c)
