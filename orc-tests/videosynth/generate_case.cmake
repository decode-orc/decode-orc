# Generates one videosynth case; run as a CTest fixture by
# orc_videosynth_case() in this directory's CMakeLists.txt.
#
#   cmake -DVIDEOSYNTH=<binary> -DPROJECT=<yaml> -DOUTPUT_DIR=<dir>
#         -P generate_case.cmake
#
# The output is kept while its stamp matches. The stamp is the videosynth
# binary's resolved path (a nix store path, so it changes when the pin moves),
# the SHA-256 of the project file and the SHA-256 of every file it reads from
# beside itself ({project}/...), so only a pin move, a project edit or new
# media regenerates the case. videosynth's output is deterministic, so nothing
# else can change it.
#
# The generation time goes to .videosynth-generation beside the stamp, for the
# run report.

foreach(_required VIDEOSYNTH PROJECT OUTPUT_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "generate_case.cmake: ${_required} is not set")
    endif()
endforeach()

if(NOT EXISTS "${PROJECT}")
    message(FATAL_ERROR "videosynth project '${PROJECT}' does not exist. "
                        "Build the videosynth-projects target first.")
endif()

file(REAL_PATH "${VIDEOSYNTH}" _videosynth_real)
file(SHA256 "${PROJECT}" _project_hash)
set(_stamp "videosynth=${_videosynth_real}\nproject_sha256=${_project_hash}\n")
get_filename_component(_project_dir "${PROJECT}" DIRECTORY)
file(STRINGS "${PROJECT}" _media_lines REGEX "{project}/")
foreach(_line IN LISTS _media_lines)
    string(REGEX MATCH "{project}/([^\"' ]+)" _match "${_line}")
    set(_media "${_project_dir}/${CMAKE_MATCH_1}")
    if(EXISTS "${_media}")
        file(SHA256 "${_media}" _media_hash)
        string(APPEND _stamp "${CMAKE_MATCH_1}_sha256=${_media_hash}\n")
    endif()
endforeach()
set(_stamp_file "${OUTPUT_DIR}/.videosynth-stamp")

if(EXISTS "${_stamp_file}")
    file(READ "${_stamp_file}" _previous)
    if(_previous STREQUAL _stamp)
        message(STATUS "Up to date: ${OUTPUT_DIR}")
        return()
    endif()
endif()

# A stale case is removed whole, so files the new project no longer writes
# (a dropout sidecar, an audio track) cannot survive into the new output.
file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# Microseconds since the epoch. string(TIMESTAMP) reports SOURCE_DATE_EPOCH
# when it is set, as the nix dev shell sets it, so it is cleared here.
unset(ENV{SOURCE_DATE_EPOCH})
function(_now_us out)
    string(TIMESTAMP _s "%s" UTC)
    string(TIMESTAMP _us "%f" UTC)
    math(EXPR _now "${_s} * 1000000 + ${_us}")
    set(${out} ${_now} PARENT_SCOPE)
endfunction()

message(STATUS "Generating ${PROJECT} -> ${OUTPUT_DIR}")
_now_us(_started)
execute_process(
    COMMAND "${VIDEOSYNTH}" --project "${PROJECT}" --output-root "${OUTPUT_DIR}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "videosynth failed (${_result}) on ${PROJECT}:\n${_output}")
endif()

_now_us(_finished)
math(EXPR _elapsed_ms "(${_finished} - ${_started}) / 1000")
string(TIMESTAMP _when "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${OUTPUT_DIR}/.videosynth-generation"
     "generated_at=${_when}\ngeneration_ms=${_elapsed_ms}\n")

# Written last: an interrupted run leaves no stamp and is regenerated.
file(WRITE "${_stamp_file}" "${_stamp}")
