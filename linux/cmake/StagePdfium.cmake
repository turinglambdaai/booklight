# Copies libpdfium.so into the rivet stage when one is wired up.
#
# `raco rivet build` compiles the backend into .rivet/stage before
# configuring this CMake project, then copies only the host binary into the
# same stage — everything else a package ships must land there itself. The
# stage directory may be passed as -Dstage_dir=...; otherwise the default
# rivet layout is probed: the build dir is <repo>/.rivet/build/<host>, so
# the stage is <binary>/../../../stage. Absent stage → no-op, because manual
# builds stage nothing (the loader search / RIVET_PDFIUM_PATH covers them).

if(NOT stage_dir)
  get_filename_component(candidate "${binary}/../../.." ABSOLUTE)
  if(EXISTS "${candidate}/stage/runtime/racket.boot")
    set(stage_dir "${candidate}/stage")
  endif()
endif()

if(stage_dir AND EXISTS "${stage_dir}/runtime/racket.boot")
  file(COPY "${source}" DESTINATION "${stage_dir}")
  message(STATUS "staged libpdfium.so into ${stage_dir}")
else()
  message(STATUS "no rivet stage detected; libpdfium.so stays beside the binary")
endif()
