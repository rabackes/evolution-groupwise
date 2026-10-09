cmake_minimum_required(VERSION 3.16)

project(evolution-groupwise VERSION 0.8.2 LANGUAGES C)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

set(GETTEXT_PACKAGE "evolution-groupwise")
set(EDS_MIN_VERSION "3.52")

pkg_check_modules(CAMEL REQUIRED IMPORTED_TARGET camel-1.2>=${EDS_MIN_VERSION})
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.76)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0>=3.4)
pkg_check_modules(LIBXML REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(EDATASERVER REQUIRED IMPORTED_TARGET libedataserver-1.2>=${EDS_MIN_VERSION})
pkg_check_modules(EBACKEND REQUIRED IMPORTED_TARGET libebackend-1.2>=${EDS_MIN_VERSION})
pkg_check_modules(EBOOK_CONTACTS REQUIRED IMPORTED_TARGET libebook-contacts-1.2>=${EDS_MIN_VERSION})
pkg_check_modules(EDATA_BOOK REQUIRED IMPORTED_TARGET libedata-book-1.2>=${EDS_MIN_VERSION})
pkg_check_modules(ECAL REQUIRED IMPORTED_TARGET libecal-2.0>=${EDS_MIN_VERSION})
pkg_check_modules(EDATA_CAL REQUIRED IMPORTED_TARGET libedata-cal-2.0>=${EDS_MIN_VERSION})

# EDS looks for Camel providers in its own prefix and in every prefix listed in
# $EDS_EXTRA_PREFIXES. Install below CMAKE_INSTALL_PREFIX using the same relative
# layout as EDS, so a private prefix (no root needed) works for development.
pkg_get_variable(EDS_PREFIX camel-1.2 prefix)
pkg_get_variable(EDS_CAMEL_PROVIDERDIR camel-1.2 camel_providerdir)
file(RELATIVE_PATH _camel_providerdir_rel "${EDS_PREFIX}" "${EDS_CAMEL_PROVIDERDIR}")
set(GW_CAMEL_PROVIDERDIR "${CMAKE_INSTALL_PREFIX}/${_camel_providerdir_rel}")

# libegroupwise is private to the modules; they find it through their RPATH
set(GW_PRIVLIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}/evolution-groupwise")
set(CMAKE_INSTALL_RPATH "${GW_PRIVLIBDIR}")

if(NOT CMAKE_INSTALL_PREFIX STREQUAL EDS_PREFIX)
	message(STATUS "Install prefix differs from EDS prefix (${EDS_PREFIX}): "
		"run Evolution with EDS_EXTRA_PREFIXES=${CMAKE_INSTALL_PREFIX}")
endif()

add_compile_definitions(
	GETTEXT_PACKAGE="${GETTEXT_PACKAGE}"
	LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
	G_LOG_DOMAIN="evolution-groupwise"
)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

enable_testing()

add_subdirectory(src/libegroupwise)
add_subdirectory(src/eds)
add_subdirectory(src/camel)
add_subdirectory(src/addressbook)
add_subdirectory(src/calendar)

# The account assistant page in Evolution itself (needs evolution-dev)
option(WITH_EVOLUTION_UI "Build the Evolution configuration module" ON)
if(WITH_EVOLUTION_UI)
	pkg_check_modules(EVOLUTION_MAIL REQUIRED IMPORTED_TARGET evolution-mail-3.0>=${EDS_MIN_VERSION})
	pkg_check_modules(EVOLUTION_SHELL REQUIRED IMPORTED_TARGET evolution-shell-3.0>=${EDS_MIN_VERSION})
	pkg_check_modules(EVOLUTION_CALENDAR REQUIRED IMPORTED_TARGET evolution-calendar-3.0>=${EDS_MIN_VERSION})
	pkg_check_modules(EBOOK REQUIRED IMPORTED_TARGET libebook-1.2>=${EDS_MIN_VERSION})
	add_subdirectory(src/evolution)
endif()
add_subdirectory(src/registry)
add_subdirectory(po)
add_subdirectory(tools)
add_subdirectory(tests)

message(STATUS "Camel provider dir: ${GW_CAMEL_PROVIDERDIR}")
