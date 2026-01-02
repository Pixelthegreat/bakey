workspace 'bakey'
	configurations {'debug', 'release'}

-- Split string --
function split(str, delim)
	local result = {}
	local pattern = '([^' .. delim .. ']+)'
	for match in string.gmatch(str, pattern) do
		table.insert(result, match)
	end
	return result
end

-- Table contains value --
function contains_value(table, value)
	for _, v in pairs(table) do
		if v == value then
			return true
		end
	end
	return false
end

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

-- Implementations --
bakey_enable_implementations = {'dummy', 'sdl', 'wl'}

newoption {
	trigger = 'enable-implementations',
	value = 'IMPL1,IMPL2,...',
	description = 'Enable specific implementations only',
}

if _OPTIONS['enable-implementations'] then
	bakey_enable_implementations = split(_OPTIONS['enable-implementations'], ',')
end

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
if contains_value(bakey_enable_implementations, 'dummy') then

	project 'bakey-dummy'
		kind 'ConsoleApp'
		files {'src/bakey-dummy.c'}
		links {'bakey'}
		targetdir 'bin'
end

-- Bakey SDL backend implementation --
if contains_value(bakey_enable_implementations, 'sdl') then

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
end

-- Bakey Wayland backend implementation --
if contains_value(bakey_enable_implementations, 'wl') then

	project 'bakey-wl'
		kind 'WindowedApp'
		files {'src/bakey-wl.c',
		       'src/bakey-xdg-shell.c',
		       'include/bakey-wl-config.h',
		       'include/bakey-xdg-shell.h'}
		optimize 'Speed'
		buildoptions {'`pkg-config --cflags freetype2`'}
		links {'bakey', 'wayland-client', 'xkbcommon'}
		linkoptions {'`pkg-config --libs freetype2`'}
		targetdir 'bin'

		defines {'_DEFAULT_SOURCE'}

		filter 'options:enable-bakey-rc'
			links {'bakey-rc'}
			defines {'BAKEY_RC'}
end
