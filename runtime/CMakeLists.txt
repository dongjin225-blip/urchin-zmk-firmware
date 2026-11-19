if(CONFIG_ZMK_RUNTIME_EDITOR)
  set(runtime_mode enabled)
else()
  set(runtime_mode disabled)
endif()
execute_process(COMMAND ${PYTHON_EXECUTABLE}
    ${CMAKE_CURRENT_LIST_DIR}/patch_zmk.py
    ${ZEPHYR_BASE}/../zmk ${ZEPHYR_ZMK_STUDIO_MESSAGES_MODULE_DIR}
    ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_BINARY_DIR} ${runtime_mode}
    COMMAND_ERROR_IS_FATAL ANY)
