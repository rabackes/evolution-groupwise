# The conversion between GroupWise calendar items and iCalendar, shared by the backend and the tests
add_library(gw-calendar STATIC
	e-gw-calendar.c
	e-gw-calendar.h
)
set_target_properties(gw-calendar PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(gw-calendar PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gw-calendar PUBLIC egroupwise PkgConfig::ECAL)

# The backend module of evolution-calendar-factory
pkg_get_variable(EDS_CAL_BACKENDDIR libedata-cal-2.0 backenddir)
file(RELATIVE_PATH _cal_backenddir_rel "${EDS_PREFIX}" "${EDS_CAL_BACKENDDIR}")
set(GW_CAL_BACKENDDIR "${CMAKE_INSTALL_PREFIX}/${_cal_backenddir_rel}")

add_library(ecalbackendgroupwise MODULE e-cal-backend-groupwise.c)
target_link_libraries(ecalbackendgroupwise PRIVATE gw-calendar egroupwise-eds PkgConfig::EDATA_CAL)

install(TARGETS ecalbackendgroupwise LIBRARY DESTINATION "${GW_CAL_BACKENDDIR}")
