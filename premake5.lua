workspace 'bakey'
	configurations {'debug', 'release'}

-- Options --
newoption {
	trigger = 'enable-bakey-rc',
	description = 'Enable RC extension',
}

newoption {
	trigger = 'prefix',
	value = 'PREFIX',
	description = 'Installation prefix (default is /usr)',
}

-- Global configurations --
language 'C'
cdialect 'C99'
includedirs {'include'}
objdir 'obj'
libdirs {'lib'}
defines {'_XOPEN_SOURCE=700'}

bakey_prefix = string.format('BAKEY_PREFIX="%s"', _OPTIONS['prefix'] or '/usr')
defines {bakey_prefix}

filter 'configurations:debug'
	defines {'DEBUG'}
	symbols 'on'

filter 'configurations:release'
	defines {'NDEBUG'}
	optimize 'on'

-- Bakey --
project 'bakey'
	kind 'StaticLib'
	files {'src/bakey.c', 'include/bakey.h', 'include/bakey-config.h'}
	targetdir 'lib'

-- Bakey RC extension --
project 'bakey-rc'
	kind 'StaticLib'
	files {'src/bakey-rc.c', 'include/bakey-rc.h'}
	targetdir 'lib'

	filter 'not options:enable-bakey-rc'
		kind 'None'

-- Bakey dummy backend implementation --
project 'bakey-dummy'
	kind 'ConsoleApp'
	files {'src/bakey-dummy.c'}
	links {'bakey'}
	targetdir 'bin'

-- Bakey SDL backend implementation --
project 'bakey-sdl'
	kind 'WindowedApp'
	files {'src/bakey-sdl.c', 'include/bakey-sdl-config.h'}
	buildoptions {'`pkg-config --cflags pangocairo`'}
	links {'bakey', 'SDL2main', 'SDL2'}
	linkoptions {'`pkg-config --libs pangocairo`'}
	targetdir 'bin'

	defines {'_DEFAULT_SOURCE'}

	filter 'options:enable-bakey-rc'
		links {'bakey-rc'}
		defines {'BAKEY_RC'}
