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
	trigger = 'enable-asan',
	description = 'Enable address sanitization',
}

newoption {
	trigger = 'enable-escape-sequence-debug',
	description = 'Enable debug printing of escape sequences and special characters',
}

newoption {
	trigger = 'enable-bakey-rc',
	description = 'Enable RC extension',
}

newoption {
	trigger = 'disable-bakey-posix',
	description = 'Disable Bakey Posix helper library (required for most implementations)',
}

newoption {
	trigger = 'prefix',
	value = 'PREFIX',
	description = 'Installation prefix (default is /usr)',
}

-- Implementations --
bakey_enable_implementations = {'dummy', 'reference', 'sdl', 'wl'}

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

filter 'options:enable-asan'
	sanitize 'Address'

-- Bakey --
project 'bakey'
	kind 'StaticLib'
	files {'src/bakey.c', 'include/bakey.h', 'include/bakey-config.h'}
	targetdir 'lib'

	filter 'options:enable-escape-sequence-debug'
		defines {'BAKEY_ESCAPE_SEQUENCE_DEBUG'}

-- Bakey RC extension --
project 'bakey-rc'
	kind 'StaticLib'
	files {'src/bakey-rc.c', 'include/bakey-rc.h'}
	targetdir 'lib'

	filter 'not options:enable-bakey-rc'
		kind 'None'

-- Bakey Posix helper library --
project 'bakey-posix'
	kind 'StaticLib'
	files {'src/bakey-posix.c', 'include/bakey-posix.h'}
	targetdir 'lib'

	filter 'options:disable-bakey-posix'
		kind 'None'

-- Escape sequence tests --
project 'test'
	kind 'ConsoleApp'
	files {'src/test.c'}
	targetdir 'bin'

-- Implementations --
for i, impl in ipairs(bakey_enable_implementations) do
	include('impl-' .. impl .. '.lua')
end
