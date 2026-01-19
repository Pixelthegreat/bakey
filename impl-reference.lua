-- Bakey SDL simplified reference implementation --
project 'bakey-reference'
	kind 'WindowedApp'
	files {'src/bakey-reference.c', 'include/bakey-reference.h'}
	links {'bakey', 'SDL2main', 'SDL2'}
	targetdir 'bin'

	defines {'_DEFAULT_SOURCE'}
