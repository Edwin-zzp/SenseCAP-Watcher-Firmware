idf_component_register(
    SRCS "camera_uploader.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "../factory_firmware/main"
        "../factory_firmware/main/task_flow_module"
        "../factory_firmware/main/task_flow_module/common"
)
