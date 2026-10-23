# Save sync (Core/SaveSync.h) with emusync (https://github.com/ilyalissoboi/emusync). PPSSPP loads
# the library at run time and builds against the vendored header (ext/emusync), so this only puts
# the library next to the program, from a pinned release (no Rust needed):
#   - Linux x86_64 and macOS: downloads emusync-<version>-<platform>.tar.gz into the build folder and
#     checks its SHA-256 (from the release's SHA256SUMS); emusync_deploy_runtime(<target>) copies the
#     library next to the program (into the app bundle's Frameworks on macOS).
#   - Android: the app's Gradle build packages the library from emusync's AAR.
#   - Windows: Windows/EmuSync.props.
# Anything else, -DUSE_EMUSYNC=OFF, or a download that fails: PPSSPP runs without save sync.
# Keep EMUSYNC_VERSION in step with ext/emusync and Windows/EmuSync.props.

option(USE_EMUSYNC "Save sync with emusync" ON)
set(EMUSYNC_VERSION 0.2.1)
set(EMUSYNC_SHA256_LINUX_X86_64 "0a7339739a9bb4bacda58234fbbb5a0b25606c7fad6309243292c666a93be09e")
set(EMUSYNC_SHA256_MACOS "9d8e8c66ae92ec61ae7239a53f762295c16bcdee34f65181135286a391193778")
set(EMUSYNC_RUNTIME "")

if(USE_EMUSYNC AND NOT (IOS OR LIBRETRO OR WIN32 OR ANDROID))
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
		if(NOT EXISTS ${_emusync_dir}/lib/${_emusync_library})
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
				message(WARNING "emusync: download failed (${_emusync_status}); PPSSPP runs without save sync")
			endif()
		endif()
		if(EXISTS ${_emusync_dir}/lib/${_emusync_library})
			set(EMUSYNC_RUNTIME ${_emusync_dir}/lib/${_emusync_library})
		endif()
	endif()
endif()
message(STATUS "emusync (save sync) library: ${EMUSYNC_RUNTIME}")

# Copies the library next to the program, where SaveSync looks for it (the app bundle's Frameworks
# folder on macOS).
function(emusync_deploy_runtime target)
	if(NOT EMUSYNC_RUNTIME)
		return()
	endif()
	get_target_property(_bundle ${target} MACOSX_BUNDLE)
	if(APPLE AND _bundle)
		set(_dest "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
	else()
		set(_dest "$<TARGET_FILE_DIR:${target}>")
	endif()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${EMUSYNC_RUNTIME}" "${_dest}"
		VERBATIM)
endfunction()
