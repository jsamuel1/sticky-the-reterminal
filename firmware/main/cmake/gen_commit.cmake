# Build-time (NOT configure-time) git sha injection.
# Configure-time injection goes stale on incremental builds, which is exactly
# the version-collision class this retires (afc46de1 claimed 0.14.8 while
# missing a later commit). Runs every build; writes only when changed, so an
# unchanged sha causes zero rebuilds.
execute_process(
  COMMAND git describe --always --abbrev=8
  WORKING_DIRECTORY ${REPO_DIR}
  OUTPUT_VARIABLE TINY_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE RC)
if(NOT RC EQUAL 0 OR TINY_SHA STREQUAL "")
  set(TINY_SHA "nogit")
endif()
# Dirty flag scoped to firmware/: uncommitted markdown elsewhere in the repo
# must not taint the FIRMWARE's provenance
# string ("e1fac273-dirty" for a clean-source build). The artifact's honesty
# boundary is the tree that is COMPILED INTO it -- firmware/ -- not the repo.
execute_process(
  COMMAND git status --porcelain -- ${REPO_DIR}
  WORKING_DIRECTORY ${REPO_DIR}
  OUTPUT_VARIABLE TINY_DIRTY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT TINY_DIRTY STREQUAL "" AND NOT TINY_SHA STREQUAL "nogit")
  set(TINY_SHA "${TINY_SHA}-dirty")
endif()
# Commit date (committer, UTC) of the sha above: the glass shows WHEN the
# firmware's source was cut. Not the compile time: esp_app_desc's date/time is
# only refreshed when esp_app_format recompiles, so incremental builds carry a
# stale stamp (two bench sets of different commits read 23:32:57 alike).
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env TZ=UTC git -c log.showSignature=false log -1 --format=%cd --date=format-local:%Y-%m-%d\ %H:%M
  WORKING_DIRECTORY ${REPO_DIR}
  OUTPUT_VARIABLE TINY_DATE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE RC2)
if(NOT RC2 EQUAL 0 OR TINY_DATE STREQUAL "")
  set(TINY_DATE "?")
else()
  set(TINY_DATE "${TINY_DATE} UTC")
endif()
set(CONTENT "#pragma once\n#define TINY_FW_COMMIT \"${TINY_SHA}\"\n#define TINY_FW_COMMIT_DATE \"${TINY_DATE}\"\n")
set(OLD "")
if(EXISTS ${OUT})
  file(READ ${OUT} OLD)
endif()
if(NOT OLD STREQUAL CONTENT)
  file(WRITE ${OUT} "${CONTENT}")
endif()
