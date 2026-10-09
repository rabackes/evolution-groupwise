# Evolution loads its modules from here, also below EDS_EXTRA_PREFIXES
pkg_get_variable(EVOLUTION_MODULEDIR evolution-shell-3.0 moduledir)
pkg_get_variable(EVOLUTION_PREFIX evolution-shell-3.0 prefix)
file(RELATIVE_PATH _evolution_moduledir_rel "${EVOLUTION_PREFIX}" "${EVOLUTION_MODULEDIR}")
set(GW_EVOLUTION_MODULEDIR "${CMAKE_INSTALL_PREFIX}/${_evolution_moduledir_rel}")

add_library(module-groupwise-configuration MODULE module-groupwise-configuration.c e-groupwise-source-config.c)
set_target_properties(module-groupwise-configuration PROPERTIES PREFIX "")

# The mail view actions use the EUIManager of Evolution 3.56 (before: GtkUIManager)
if(EVOLUTION_SHELL_VERSION VERSION_GREATER_EQUAL 3.55.1)
	target_sources(module-groupwise-configuration PRIVATE e-groupwise-mail-ui.c e-groupwise-settings-window.c
		e-groupwise-access-tab.c e-groupwise-junk-tab.c e-groupwise-proxy-tab.c e-groupwise-rule-editor.c e-groupwise-rule-runner.c e-groupwise-rules-tab.c e-groupwise-sent-items.c e-groupwise-signature-sync.c e-groupwise-signatures-tab.c e-groupwise-travel-page.c e-groupwise-vacation-tab.c e-groupwise-ui-utils.c)
	target_compile_definitions(module-groupwise-configuration PRIVATE GW_HAVE_MAIL_UI)
else()
	message(STATUS "Evolution ${EVOLUTION_SHELL_VERSION}: no GroupWise actions in the mail view (need 3.56)")
endif()
target_link_libraries(module-groupwise-configuration PRIVATE egroupwise-eds PkgConfig::EVOLUTION_MAIL PkgConfig::EVOLUTION_SHELL PkgConfig::EVOLUTION_CALENDAR PkgConfig::EBOOK)

install(TARGETS module-groupwise-configuration LIBRARY DESTINATION "${GW_EVOLUTION_MODULEDIR}")
