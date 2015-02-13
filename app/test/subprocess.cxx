#include "subprocess.h"

#include <unistd.h>
#include <iostream>
#include <cstring>
#include <cassert>
#ifdef _WIN32
#include <fcntl.h>
#else
#include <csignal>
#include <sys/wait.h>
#endif

#include "../../ren-cxx-basics/error.h"

SubprocessT::SubprocessT(
	asio::io_service &Service,
	Filesystem::PathT const &Executable, 
	std::vector<std::string> const &Arguments) : In(Service), Out(Service)
{
	{
		std::cout << "Running \"" << Executable << "\" with arguments: ";
		for (auto &Argument : Arguments) std::cout << "\"" << Argument << "\" ";
		std::cout << std::endl;
	}

	fflush(nullptr); // Write everything before forking so buffered data isn't written twice
#ifdef _WIN32
	HANDLE ChildInHandle = NULL;
	HANDLE ParentOutHandle = NULL;
	HANDLE ParentInHandle = NULL;
	HANDLE ChildOutHandle = NULL;

	SECURITY_ATTRIBUTES SecurityAttributes; 
	SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES); 
	SecurityAttributes.bInheritHandle = TRUE; 
	SecurityAttributes.lpSecurityDescriptor = NULL; 

	if (!CreatePipe(&ParentInHandle, &ChildOutHandle, &SecurityAttributes, 0)) 
		throw SystemErrorT() << 
			"Failed to create parent read pipe: error number " << GetLastError();

	if (!SetHandleInformation(ParentInHandle, HANDLE_FLAG_INHERIT, 0))
		throw SystemErrorT() << 
			"Failed to make parent read pipe uninheritable: error number " + GetLastError();

	if (!CreatePipe(&ChildInHandle, &ParentOutHandle, &SecurityAttributes, 0)) 
		throw SystemErrorT() << 
			"Failed to create parent write pipe: error number " + GetLastError();

	if (!SetHandleInformation(ParentOutHandle, HANDLE_FLAG_INHERIT, 0))
		throw SystemErrorT() << 
			"Failed to make parent write pipe uninheritable: error number " + GetLastError();
	
	STARTUPINFOW ChildStartupInformation;
	memset(&ChildStartupInformation, 0, sizeof(STARTUPINFO));
	ChildStartupInformation.cb = sizeof(STARTUPINFO); 
	ChildStartupInformation.hStdOutput = ChildOutHandle;
	ChildStartupInformation.hStdInput = ChildInHandle;
	if (Verbose)
		ChildStartupInformation.hStdError = ChildOutHandle;
		//ChildStartupInformation.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	ChildStartupInformation.dwFlags |= STARTF_USESTDHANDLES;	
 
	std::string ArgumentConcatenation; 
	ArgumentConcatenation << "\"" << Executeable << "\"";
	for (auto &Argument : Arguments) ArgumentConcatenation << " \"" << Argument << "\"";
	Nativestd::string NativeArguments = ToNativeString(ArgumentConcatenation);
	std::vector<wchar_t> NativeArgumentsWritableBuffer;
	NativeArgumentsWritableBuffer.resize(NativeArguments.length() + 1);
	std::copy(NativeArguments.begin(), NativeArguments.end(), NativeArgumentsWritableBuffer.begin());
	NativeArgumentsWritableBuffer[NativeArguments.length()] = 0;
	
	memset(&ChildStatus, 0, sizeof(PROCESS_INFORMATION));
	
	bool Result = CreateProcessW(
		reinterpret_cast<wchar_t const *>(ToNativeString(Executable).c_str()), 
		&NativeArgumentsWritableBuffer[0], 
		nullptr, 
		nullptr, 
		true, 
		0, 
		nullptr, 
		nullptr, 
		&ChildStartupInformation, 
		&ChildStatus);
	if (!Result) throw SystemErrorT() << 
		"Failed to spawn child process with name '" << Executable <<
		"' and arguments '" << ArgumentConcatenation.str() << 
		": error number " << GetLastError();

	CloseHandle(ChildStatus.hThread);
	CloseHandle(ChildOutHandle);
	CloseHandle(ChildInHandle);
	
	int ParentIn = _open_osfhandle((intptr_t)ParentInHandle, _O_RDONLY);
	if (ParentIn == -1) throw SystemErrorT() << 
		"Failed to get a file descriptor for parent read pipe.";
	
	int ParentOut = _open_osfhandle((intptr_t)ParentOutHandle, _O_APPEND);
	if (ParentOut == -1) throw SystemErrorT() << 
		"Failed to get a file descriptor for parent write pipe.";
	
	Out.assign(ParentIn);
	In.assign(ParentOut);
#else
	const unsigned int WriteEnd = 1, ReadEnd = 0;
	int FromChild[2], ToChild[2];
	if ((pipe(FromChild) == -1) || (pipe(ToChild) == -1)) throw SystemErrorT() << 
		"Error: Failed to create pipes for communication with controller.";

	ChildID = fork();
	if (ChildID == -1) throw SystemErrorT() << 
		"Failed to create process for controller.";

	if (ChildID == 0) // Child side
	{
		close(ToChild[WriteEnd]);
		dup2(ToChild[ReadEnd], 0);
		close(ToChild[ReadEnd]);
		close(FromChild[ReadEnd]);
		dup2(FromChild[WriteEnd], 1);
		close(FromChild[WriteEnd]);

		auto ExecutableString = Executable.Render();
		std::vector<char *> ArgPointers;
		ArgPointers.reserve(Arguments.size() + 2);
		ArgPointers.push_back(const_cast<char *>(ExecutableString.c_str()));
		for (auto &Argument : Arguments) ArgPointers.push_back(const_cast<char *>(Argument.c_str()));
		ArgPointers.push_back(nullptr);
		execv(ExecutableString.c_str(), &ArgPointers[0]);
	}
	else // Parent side
	{
		close(ToChild[ReadEnd]);
		In.assign(ToChild[WriteEnd]);

		close(FromChild[WriteEnd]);
		Out.assign(FromChild[ReadEnd]);
	}
#endif
}

SubprocessT::~SubprocessT(void) 
{
#ifdef _WIN32
	CloseHandle(ChildStatus.hProcess);
#endif
}

void SubprocessT::Terminate(void) 
{
#ifdef _WIN32
	TerminateProcess(ChildStatus.hProcess, 1);
#else
	kill(ChildID, SIGTERM); 
#endif
}

int SubprocessT::GetResult(void)
{
	if (!Result) 
	{
#ifdef _WIN32
		WaitForSingleObject(ChildStatus.hProcess, 0);
		DWORD ReturnCode = 0;
		if (!GetExitCodeProcess(ChildStatus.hProcess, &ReturnCode))
			throw SystemErrorT() << 
				"Lost control of child process, can't get return value: error code " << 
				GetLastError();
		Result = ReturnCode;
#else
		int RawStatus = 1;
		waitpid(ChildID, &RawStatus, 0);
		if (!WIFEXITED(RawStatus))
			return 1; // Some error, pretend like command failed
		Result = WEXITSTATUS(RawStatus);
#endif
		std::cout << "Execution finished with code " << Result << "." << std::endl;
	}
	return Result;
}

