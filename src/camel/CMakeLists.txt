add_library(camelgroupwise MODULE
	camel-groupwise-folder.c
	camel-groupwise-folder.h
	camel-groupwise-labels.c
	camel-groupwise-labels.h
	camel-groupwise-message-info.c
	camel-groupwise-message-info.h
	camel-groupwise-mime.c
	camel-groupwise-mime.h
	camel-groupwise-provider.c
	camel-groupwise-store.c
	camel-groupwise-store.h
	camel-groupwise-store-summary.c
	camel-groupwise-store-summary.h
	camel-groupwise-transport.c
	camel-groupwise-transport.h
	camel-groupwise-utils.c
	camel-groupwise-utils.h
)
target_link_libraries(camelgroupwise PRIVATE egroupwise egroupwise-eds PkgConfig::CAMEL)

install(TARGETS camelgroupwise LIBRARY DESTINATION "${GW_CAMEL_PROVIDERDIR}")
install(FILES libcamelgroupwise.urls DESTINATION "${GW_CAMEL_PROVIDERDIR}")
