-- Bakey Wayland backend implementation --
project 'bakey-wl'
	kind 'WindowedApp'
	files {'src/bakey-wl.c',
	       'src/bakey-xdg-shell.c',
	       'include/bakey-wl-config.h',
	       'include/bakey-xdg-shell.h'}
	optimize 'Speed'
	buildoptions {'`pkg-config --cflags freetype2`'}
	links {'bakey', 'bakey-posix', 'wayland-client', 'xkbcommon'}
	linkoptions {'`pkg-config --libs freetype2`'}
	targetdir 'bin'

	defines {'_DEFAULT_SOURCE'}

	filter 'options:enable-bakey-rc'
		links {'bakey-rc'}
		defines {'BAKEY_RC'}
