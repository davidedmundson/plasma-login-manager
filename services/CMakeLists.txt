if(SYSTEMD_FOUND)
    configure_file(plasmalogin.service.in plasmalogin.service)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasmalogin.service" DESTINATION "${SYSTEMD_SYSTEM_UNIT_DIR}")

    configure_file(plasmalogin-sysuser.conf.in plasmalogin-sysuser.conf)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasmalogin-sysuser.conf" DESTINATION "${SYSTEMD_SYSUSERS_DIR}" RENAME plasmalogin.conf)
endif()

# systemd-tmpfiles can be used standalone without other systemd parts
if(DEFINED SYSTEMD_TMPFILES_DIR)
    configure_file(plasmalogin-tmpfiles.conf.in plasmalogin-tmpfiles.conf)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasmalogin-tmpfiles.conf" DESTINATION "${SYSTEMD_TMPFILES_DIR}" RENAME plasmalogin.conf)
endif()
