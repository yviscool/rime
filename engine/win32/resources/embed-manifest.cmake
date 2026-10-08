# Embeds visual-styles.manifest into an executable after it has been linked.
#
# Invoked as
#   cmake -DRIME_MT=<mt.exe> -DRIME_MANIFEST=<visual-styles.manifest>
#         -DRIME_EXE=<output.exe> -P embed-manifest.cmake
#
# The -outputresource argument needs a literal ";" between the path and the
# resource id. Handing that straight to add_custom_command makes the command
# writer escape the ";" for the generator and mt.exe receives a path ending
# in a backslash, so the argument is assembled here instead.
if (NOT RIME_MT OR NOT RIME_MANIFEST OR NOT RIME_EXE)
  message(FATAL_ERROR "embed-manifest.cmake needs RIME_MT, RIME_MANIFEST and RIME_EXE")
endif()

execute_process(
  COMMAND "${RIME_MT}" -nologo -manifest "${RIME_MANIFEST}"
          "-outputresource:${RIME_EXE};1"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if (NOT result EQUAL 0)
  message(FATAL_ERROR "mt.exe failed (${result}): ${output}${error}")
endif()
