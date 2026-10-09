# The module of evolution-source-registry: the GroupWise collection
pkg_get_variable(EDS_REGISTRY_MODULEDIR libebackend-1.2 moduledir)
file(RELATIVE_PATH _registry_moduledir_rel "${EDS_PREFIX}" "${EDS_REGISTRY_MODULEDIR}")
set(GW_REGISTRY_MODULEDIR "${CMAKE_INSTALL_PREFIX}/${_registry_moduledir_rel}")

add_library(module-groupwise-backend MODULE module-groupwise-backend.c gw-signatures.c)
set_target_properties(module-groupwise-backend PROPERTIES PREFIX "")
target_link_libraries(module-groupwise-backend PRIVATE egroupwise-eds PkgConfig::EBACKEND)

install(TARGETS module-groupwise-backend LIBRARY DESTINATION "${GW_REGISTRY_MODULEDIR}")
