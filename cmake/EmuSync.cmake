# Save sync (Core/SaveSync.h) with emusync (https://github.com/ilyalissoboi/emusync), from a pinned
# release, so building PPSSPP needs no Rust:
#   - Linux x86_64 and macOS: downloads emusync-<version>-<platform>.tar.gz into the build folder,
#     checks its SHA-256 (from the release's SHA256SUMS), and defines the imported target
#     emusync::emusync. emusync_deploy_runtime(<target>) puts the library next to the program.
#   - Android: the app's Gradle build adds emusync's AAR, whose Prefab package find_package() finds.
# Anything else (Windows builds through Windows/EmuSync.props instead), -DUSE_EMUSYNC=OFF, or a
# download that fails: EMUSYNC_FOUND is OFF, and Core/SaveSync.cpp compiles to no-ops.

option(USE_EMUSYNC "Save sync with emusync" ON)
set(EMUSYNC_FOUND OFF)
set(EMUSYNC_VERSION 0.2.0)
set(EMUSYNC_SHA256_LINUX_X86_64 "ae56a1a86a319641e442622e5ccd05edcae533972999aa9b0aa43e4bb92d1b37")
set(EMUSYNC_SHA256_MACOS "3f6a65d23123ba6f82ce5b87bebbd4fb7fc0880bff04e0323210a7a5f8abfad1")

if(NOT USE_EMUSYNC OR IOS OR LIBRETRO OR WIN32)
	# Not here.
elseif(ANDROID)
	find_package(emusync CONFIG QUIET)
	if(TARGET emusync::emusync)
		set(EMUSYNC_FOUND ON)
	endif()
else()
	set(_emusync_package "")
	if(APPLE)
		set(_emusync_package macos-universal)
		set(_emusync_sha256 ${EMUSYNC_SHA256_MACOS})
		set(_emusync_library libemusync.dylib)
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
		set(_emusync_package linux-x86_64)
		set(_emusync_sha256 ${EMUSYNC_SHA256_LINUX_X86_64})
		set(_emusync_library libemusync.so)
	endif()
	if(_emusync_package)
		set(_emusync_name emusync-${EMUSYNC_VERSION}-${_emusync_package})
		set(_emusync_dir ${CMAKE_BINARY_DIR}/${_emusync_name})
		if(NOT EXISTS ${_emusync_dir}/include/emusync.h)
			message(STATUS "emusync: downloading ${_emusync_name}.tar.gz")
			file(DOWNLOAD
				https://github.com/ilyalissoboi/emusync/releases/download/v${EMUSYNC_VERSION}/${_emusync_name}.tar.gz
				${_emusync_dir}.tar.gz
				EXPECTED_HASH SHA256=${_emusync_sha256}
				STATUS _emusync_status)
			list(GET _emusync_status 0 _emusync_code)
			if(_emusync_code EQUAL 0)
				# cmake -E tar: file(ARCHIVE_EXTRACT) needs CMake 3.18, PPSSPP 3.16.
				file(MAKE_DIRECTORY ${_emusync_dir})
				execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${_emusync_dir}.tar.gz
					WORKING_DIRECTORY ${_emusync_dir})
			else()
				message(WARNING "emusync: download failed (${_emusync_status}); building without save sync")
			endif()
		endif()
		if(EXISTS ${_emusync_dir}/include/emusync.h)
			add_library(emusync::emusync SHARED IMPORTED)
			set_target_properties(emusync::emusync PROPERTIES
				IMPORTED_LOCATION ${_emusync_dir}/lib/${_emusync_library}
				INTERFACE_INCLUDE_DIRECTORIES ${_emusync_dir}/include)
			set(EMUSYNC_RUNTIME ${_emusync_dir}/lib/${_emusync_library})
			set(EMUSYNC_FOUND ON)
		endif()
	else()
		message(STATUS "emusync: no prebuilt library for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}; building without save sync")
	endif()
endif()
message(STATUS "emusync (save sync): ${EMUSYNC_FOUND}")

# Copies the library next to the program and points the program's rpath there (into the app
# bundle's Frameworks folder on macOS). The library's own install name is @rpath/libemusync.dylib,
# its soname libemusync.so.
function(emusync_deploy_runtime target)
	if(NOT EMUSYNC_FOUND OR ANDROID)
		return()
	endif()
	get_target_property(_bundle ${target} MACOSX_BUNDLE)
	if(APPLE AND _bundle)
		set(_dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
		set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
	else()
		set(_dest "$<TARGET_FILE_DIR:${target}>")
		if(APPLE)
			set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@executable_path")
		else()
			set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
		endif()
	endif()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${EMUSYNC_RUNTIME}" "${_dest}"
		VERBATIM)
endfunction()
