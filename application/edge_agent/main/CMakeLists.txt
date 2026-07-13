file(GLOB MAIN_C_SRCS "app_fs.c" "main.c" "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
set(EXTRA_CC_SRCS "")
set(EXTRA_PRIV_REQUIRES "")
set(EXTRA_INCLUDE_DIRS "")

if(CONFIG_SPI_CAM_BRIDGE_ENABLE AND CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    list(APPEND EXTRA_PRIV_REQUIRES lua_module_camera esp_new_jpeg)

    if(CONFIG_PERSON_DETECT_ENABLE)
        list(APPEND EXTRA_PRIV_REQUIRES esp-tflite-micro)
        list(APPEND EXTRA_CC_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/person_detect.cc")

        # Model data CC/H are in examples/person_detection/main/ (not in a model/ subdir)
        set(TFLITE_MC_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-tflite-micro")
        set(TFLITE_MODEL_CC "${TFLITE_MC_DIR}/examples/person_detection/main/person_detect_model_data.cc")
        set(TFLITE_MODEL_H  "${TFLITE_MC_DIR}/examples/person_detection/main")

        if(EXISTS "${TFLITE_MODEL_CC}")
            list(APPEND EXTRA_CC_SRCS "${TFLITE_MODEL_CC}")
            list(APPEND EXTRA_INCLUDE_DIRS "${TFLITE_MODEL_H}")
        else()
            message(WARNING
                "person_detect_model_data.cc not found at:\n  ${TFLITE_MODEL_CC}\n"
                "Run 'idf.py reconfigure' to download esp-tflite-micro managed component first.")
        endif()
    endif()
endif()

idf_component_register(
    SRCS ${MAIN_C_SRCS} ${EXTRA_CC_SRCS}
    INCLUDE_DIRS "." ${EXTRA_INCLUDE_DIRS}
    PRIV_REQUIRES ${EXTRA_PRIV_REQUIRES}
)
