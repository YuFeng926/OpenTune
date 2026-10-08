# ==============================================================================
# Removes stale ONNX Runtime dylibs from a macOS bundle's Contents/Frameworks
# directory. Invoked as a POST_BUILD step so that after an ORT version change
# old libonnxruntime.<old>.dylib copies do not accumulate and get shipped.
#
# Required definitions:
#   ORT_FRAMEWORKS_DIR - bundle Contents/Frameworks directory to clean
#   ORT_KEEP_FILENAME  - file name of the pinned dylib to keep
# ==============================================================================

if(NOT DEFINED ORT_FRAMEWORKS_DIR OR NOT DEFINED ORT_KEEP_FILENAME)
    message(FATAL_ERROR
        "CleanStaleOrtDylibs.cmake requires ORT_FRAMEWORKS_DIR and ORT_KEEP_FILENAME")
endif()

file(GLOB _opentune_ort_dylibs "${ORT_FRAMEWORKS_DIR}/libonnxruntime.*.dylib")
foreach(_opentune_ort_dylib IN LISTS _opentune_ort_dylibs)
    get_filename_component(_opentune_ort_name "${_opentune_ort_dylib}" NAME)
    if(NOT _opentune_ort_name STREQUAL ORT_KEEP_FILENAME)
        message(STATUS "Removing stale ONNX Runtime dylib: ${_opentune_ort_dylib}")
        file(REMOVE "${_opentune_ort_dylib}")
    endif()
endforeach()
