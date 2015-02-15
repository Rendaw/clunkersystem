DoOnce 'ren-cxx-filesystem/Tupfile.lua'
DoOnce 'ren-cxx-subprocess/Tupfile.lua'

Define.Executable
{
	Name = 'test_everything',
	Sources = Item() + 'test_everything.cxx',
	Objects = Item() + FilesystemObjects + SubprocessObjects,
	BuildFlags = '-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse',
	LinkFlags = '-lfuse -pthread -lluxem-cxx',
}

