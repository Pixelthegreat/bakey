-- Bakey SDL backend implementation --
project 'bakey-sdl'
	kind 'WindowedApp'
	files {'src/bakey-sdl.c', 'include/bakey-sdl-config.h'}
	buildoptions {'`pkg-config --cflags freetype2`'}
	links {'bakey', 'bakey-posix', 'SDL2main', 'SDL2', 'm'}
	linkoptions {'`pkg-config --libs freetype2`'}
	targetdir 'bin'

	defines {'_DEFAULT_SOURCE'}

	filter 'options:enable-bakey-rc'
		links {'bakey-rc'}
		defines {'BAKEY_RC'}
