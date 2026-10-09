add_library(egroupwise SHARED
	e-gw-addressbook.c
	e-gw-addressbook.h
	e-gw-category.c
	e-gw-category.h
	e-gw-connection.c
	e-gw-connection.h
	e-gw-folder.c
	e-gw-folder.h
	e-gw-junk.c
	e-gw-junk.h
	e-gw-proxy.c
	e-gw-proxy.h
	e-gw-rule.c
	e-gw-rule.h
	e-gw-signature.c
	e-gw-signature.h
	e-gw-vacation.c
	e-gw-vacation.h
	e-gw-xml.c
	e-gw-xml.h
)
target_include_directories(egroupwise PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(egroupwise
	PUBLIC PkgConfig::GIO PkgConfig::LIBXML
	PRIVATE PkgConfig::SOUP
)
target_compile_definitions(egroupwise PRIVATE PACKAGE_VERSION="${PROJECT_VERSION}")

# Private library of the modules, not a public API
install(TARGETS egroupwise LIBRARY DESTINATION "${GW_PRIVLIBDIR}")
