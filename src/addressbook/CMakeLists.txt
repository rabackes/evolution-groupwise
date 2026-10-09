# The conversion between GroupWise items and vCards, shared by the backend and the tests
add_library(gw-contact STATIC
	e-gw-contact.c
	e-gw-contact.h
)
set_target_properties(gw-contact PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gw-contact PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gw-contact PUBLIC egroupwise PkgConfig::EBOOK_CONTACTS)

# The backend module of evolution-addressbook-factory
pkg_get_variable(EDS_BOOK_BACKENDDIR libedata-book-1.2 backenddir)
file(RELATIVE_PATH _book_backenddir_rel "${EDS_PREFIX}" "${EDS_BOOK_BACKENDDIR}")
set(GW_BOOK_BACKENDDIR "${CMAKE_INSTALL_PREFIX}/${_book_backenddir_rel}")

add_library(ebookbackendgroupwise MODULE e-book-backend-groupwise.c)
target_compile_definitions(ebookbackendgroupwise PRIVATE GW_BOOK_BACKENDDIR="${GW_BOOK_BACKENDDIR}")
target_link_libraries(ebookbackendgroupwise PRIVATE gw-contact egroupwise-eds PkgConfig::EDATA_BOOK)

install(TARGETS ebookbackendgroupwise LIBRARY DESTINATION "${GW_BOOK_BACKENDDIR}")
