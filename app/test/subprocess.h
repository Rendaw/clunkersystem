#ifndef subprocess_h
#define subprocess_h

#include <vector>
#include <asio.hpp>

#include "../../ren-cxx-filesystem/path.h"

struct SubprocessT
{
	asio::posix::stream_descriptor In, Out;

	SubprocessT(
		asio::io_service &Service,
		Filesystem::PathT const &Executable, 
		std::vector<std::string> const &Arguments);
	~SubprocessT(void);

	void Terminate(void);
	int GetResult(void);

	private:
#ifdef _WIN32
		PROCESS_INFORMATION ChildStatus;
#else
		pid_t ChildID;
#endif
		OptionalT<int> Result;
};

#endif // SHARED_H
