# Translations: po/<lang>.po -> <localedir>/<lang>/LC_MESSAGES/evolution-groupwise.mo
# "make update-po" refreshes the template and merges it into the translations.
find_program(MSGFMT msgfmt REQUIRED)
find_program(XGETTEXT xgettext)
find_program(MSGMERGE msgmerge)

file(STRINGS LINGUAS _languages)
set(_mo_files)
foreach(lang IN LISTS _languages)
	set(_mo "${CMAKE_CURRENT_BINARY_DIR}/${lang}.mo")
	add_custom_command(OUTPUT "${_mo}"
		COMMAND "${MSGFMT}" --check -o "${_mo}" "${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po"
		DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${lang}.po")
	install(FILES "${_mo}" DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES" RENAME "${GETTEXT_PACKAGE}.mo")
	list(APPEND _mo_files "${_mo}")
endforeach()
add_custom_target(translations ALL DEPENDS ${_mo_files})

if(XGETTEXT AND MSGMERGE)
	add_custom_target(update-po
		COMMAND find src -name "*.c" | sort > po/POTFILES
		COMMAND "${XGETTEXT}" --from-code=UTF-8 -k_ -kN_ -kC_:1c,2 -kNC_:1c,2 --package-name=${GETTEXT_PACKAGE}
			--msgid-bugs-address=rbackes@bond.de -o po/${GETTEXT_PACKAGE}.pot -f po/POTFILES
		COMMAND sh -c "for l in $(cat po/LINGUAS); do msgmerge --update --backup=none po/$l.po po/${GETTEXT_PACKAGE}.pot; done"
		WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
		VERBATIM)
endif()
