# Shared by the Camel provider, the registry module and the backends: types
# such as CamelGroupwiseSettings may be registered only once per process.
add_library(egroupwise-eds SHARED
	camel-groupwise-settings.c
	camel-groupwise-settings.h
	e-gw-backend-utils.c
	e-gw-backend-utils.h
	e-gw-signature-mime.c
	e-gw-signature-mime.h
	e-source-groupwise-folder.c
	e-source-groupwise-folder.h
)
target_include_directories(egroupwise-eds PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(egroupwise-eds PUBLIC egroupwise PkgConfig::CAMEL PkgConfig::EDATASERVER)

install(TARGETS egroupwise-eds LIBRARY DESTINATION "${GW_PRIVLIBDIR}")
