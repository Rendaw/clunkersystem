tup.definerule
{
	inputs = {'main.cxx'},
	outputs = {'main'},
	command = 'g++ -Wall main.cxx `pkg-config fuse --cflags --libs` -lulockmgr -o main',
}

