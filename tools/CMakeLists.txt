include(CheckPIESupported)
check_pie_supported()

add_executable(gw-cli gw-cli.c)
# Installed with the package: position independent, as distributions want it
set_target_properties(gw-cli PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(gw-cli PRIVATE egroupwise PkgConfig::GIO_UNIX)

install(TARGETS gw-cli RUNTIME DESTINATION "${GW_PRIVLIBDIR}")

# Not installed: a development check of the Camel provider against a real POA
add_executable(gw-camel-check gw-camel-check.c)
target_link_libraries(gw-camel-check PRIVATE PkgConfig::CAMEL)

# Not installed: changes the mailbox, for test accounts only
add_executable(gw-camel-writetest gw-camel-writetest.c)
target_link_libraries(gw-camel-writetest PRIVATE PkgConfig::CAMEL)

# Not installed: a GroupWise collection through the real evolution-data-server
add_executable(gw-eds-check gw-eds-check.c)
target_link_libraries(gw-eds-check PRIVATE PkgConfig::EDATASERVER PkgConfig::EBOOK_CONTACTS)

# Not installed: the calendars of a collection through the real evolution-data-server
add_executable(gw-cal-check gw-cal-check.c)
target_link_libraries(gw-cal-check PRIVATE PkgConfig::ECAL)
