#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include <asio.hpp>
#include <mutex>
#include <luxem-cxx/luxem.h>
#include <chrono>
#include <thread>
#include <fcntl.h>

#include "../ren-cxx-basics/error.h"
#include "../ren-cxx-basics/variant.h"
#include "../ren-cxx-filesystem/file.h"

#include "fuse_wrapper.h"

std::vector<function<void(void)>> SignalHandlers;

void HandleSignal(int SignalNumber)
{ 
	std::cout << "Got signal " << SignalNumber << std::endl;
	for (auto const &Handler : SignalHandlers) Handler(); 
}

struct timespec Now(void)
{
	struct timespec Out;
	clock_gettime(CLOCK_REALTIME, &Out);
	return Out;
}

struct FileT
{
	struct stat stat;
	OptionalT<std::vector<uint8_t>> Data;

	FileT(void) : stat() 
	{
		stat.st_atim = Now();
		stat.st_mtim = Now();
		stat.st_ctim = Now();
		stat.st_uid = getuid();
		stat.st_gid = getgid();
	}
};

struct FilesystemT
{
	FilesystemT(void) : Count(-1) 
	{
		auto Root = Files.emplace(std::string("/"), FileT{}).first;
		Root->second.stat.st_mode = 
			S_IFDIR |
			S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IWGRP | S_IXGRP |
			S_IROTH | S_IWOTH | S_IXOTH;
	}

	void Clean(void) { Files.clear(); }
	void SetCount(size_t Count) { this->Count = Count; }
	size_t GetCount(void) const { return Count; }

	bool DecrementCount(void)
	{
		if (Count < 0) return true;
		Count -= 1;
		if (Count == 0) return false;
		return true;
	}

#define OPER if (!DecrementCount()) return -EIO;

	int getattr(const char *path, struct stat *buf)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		*buf = Found->second.stat;
		return 0;
	}

	int opendir(const char *path, struct fuse_file_info *fi)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!CheckPermission(
			Found->second,
			(fi->flags == O_RDONLY) || (fi->flags == O_RDWR),
			(fi->flags == O_WRONLY) || (fi->flags == O_RDWR),
			false)) return -EACCES;
		if (Found->second.Data) return -ENOTDIR;
		return 0;
	}

	template <typename IteratorT>
		bool InDir(IteratorT const &Test, std::string const &Path)
	{
		return
			(Test != Files.end()) &&
			(Test->first.size() > Path.size()) &&
			(Test->first.substr(0, Path.size()) == Path);
	}

	int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
	{
		std::string Path(path);
		off_t Count = 0;
		for (auto Test = ++Files.lower_bound(Path); InDir(Test, Path); ++Test)
		{
			OPER
			if (Count < offset) continue;
			Count += 1;
			auto Filename = Test->first.substr(Path.size());
			if (Filename.find_first_of('/') != std::string::npos) continue;
			if (filler(buf, Filename.c_str(), &Test->second.stat, Count)) break;
		}
		return 0;
	}

	int mkdir(const char *path, mode_t mode)
	{
		OPER
		auto Root = Files.emplace(std::string(path), FileT{}).first;
		Root->second.stat.st_mode = 
			mode |
			S_IFDIR;
		return 0;
	}

	int rmdir(const char *path)
	{
		OPER
		std::string Path(path);
		auto Test = ++Files.lower_bound(Path);
		if (InDir(Test, Path)) return -ENOTEMPTY;
		if (Files.erase(path) != 1) return -ENOENT;
		return 0;
	}

	int create(const char *path, mode_t mode, struct fuse_file_info *fi)
	{
		OPER
		auto Root = Files.emplace(std::string(path), FileT{}).first;
		Root->second.stat.st_mode = mode;
		Root->second.stat.st_mode = 
			mode |
			S_IFREG;
		Root->second.Data = std::vector<uint8_t>();
		SetFile(fi, Root->second);
		return 0;
	}

	int utimens(const char *path, const struct timespec tv[2])
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		auto &stat = Found->second.stat;
		stat.st_atim = tv[0];
		stat.st_mtim = tv[1];
		return 0;
	}

	bool CheckPermission(FileT &File, bool Read, bool Write, bool Execute)
	{
		auto const &st_mode = File.stat.st_mode;
		auto const &st_uid = File.stat.st_uid;
		auto const &st_gid = File.stat.st_gid;
		auto const uid = getuid();
		auto const gid = getgid();
		return
			(
				!Read ||
				(
					((st_mode & S_IRUSR) && (st_uid == uid)) ||
					((st_mode & S_IRGRP) && (st_gid == gid)) ||
					(st_mode & S_IROTH)
				)
			) ||
			(
				!Write ||
				(
					((st_mode & S_IWUSR) && (st_uid == uid)) ||
					((st_mode & S_IWGRP) && (st_gid == gid)) ||
					!(st_mode & S_IWOTH)
				)
			) ||
			(
				!Execute ||
				(
					((st_mode & S_IXUSR) && (st_uid == uid)) ||
					((st_mode & S_IXGRP) && (st_gid == gid)) ||
					!(st_mode & S_IXOTH)
				)
			);
	}

	int access(const char *path, int amode)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (amode == F_OK) return 0;
		if (!CheckPermission(
			Found->second, 
			amode & R_OK,
			amode & W_OK,
			amode & X_OK)) return -EACCES;
		return 0;
	}

	int unlink(const char *path)
	{
		OPER
		if (Files.erase(path) != 1) return -ENOENT;
		return 0;
	}

	int open(const char *path, struct fuse_file_info *fi)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!CheckPermission(
			Found->second,
			(fi->flags == O_RDONLY) || (fi->flags == O_RDWR),
			(fi->flags == O_WRONLY) || (fi->flags == O_RDWR),
			false)) return -EACCES;
		SetFile(fi, Found->second);
		return 0;
	}

	int read(const char *path, char *out, size_t count, off_t start, struct fuse_file_info *fi)
	{
		OPER
		auto &File = GetFile(fi);
		size_t Good = 0;
		size_t Zero = 0;
		if ((unsigned int)start >= File.Data->size())
			Zero = count;
		else 
		{
			Good = std::max(count, File.Data->size() - start);
			Zero = count - Good;
		}
		if (Good)
			memcpy(out, &(*File.Data)[start], Good);
		if (Zero)
			memset(out + Good, 0, Zero);
		return Good;
	}

	int write(const char *path, const char *out, size_t count, off_t start, struct fuse_file_info *fi)
	{
		OPER
		auto &File = GetFile(fi);
		auto const End = start + count;
		if (File.Data->size() < End)
		{
			File.Data->resize(End);
			File.stat.st_size = End;
		}
		memcpy(&(*File.Data)[start], out, count);
		return count;
	}

	int truncate(const char *path, off_t size)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		auto &File = Found->second;
		off_t OldLength = File.Data->size();
		size_t Zero = 0;
		if (OldLength < size)
			Zero = size - OldLength;
		File.Data->resize(size);
		if (Zero) memset(&(*File.Data)[OldLength], 0, Zero);
		File.stat.st_size = size;
		return 0;
	}

	int chmod(const char *path, mode_t mode)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		Found->second.stat.st_mode = mode;
		return 0;
	}

	int chown(const char *path, uid_t uid, gid_t gid)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		Found->second.stat.st_uid = uid;
		Found->second.stat.st_gid = gid;
		return 0;
	}

	private:
		void SetFile(struct fuse_file_info *fi, FileT &File)
		{
			fi->fh = reinterpret_cast<uint64_t>(&File);
		}

		FileT &GetFile(struct fuse_file_info *fi)
		{
			return *reinterpret_cast<FileT *>(fi->fh);
		}

		int64_t Count;

		std::map<std::string, FileT> Files;
};

template <typename CallbackT>
	void Accept(
		asio::io_service &Service, 
		asio::ip::tcp::acceptor &Acceptor, 
		CallbackT &&Callback,
		size_t RetryCount = 0)
{
	std::shared_ptr<asio::ip::tcp::socket> Connection;
	try
	{
		Connection = std::make_shared<asio::ip::tcp::socket>(Service);
	}
	catch (...)
	{
		if (RetryCount >= 5) throw;
		auto Retry = std::make_shared<asio::basic_waitable_timer<std::chrono::system_clock>>(Service, std::chrono::minutes(1));
		auto &RetryRef = *Retry;
		RetryRef.async_wait([&, Retry = std::move(Retry), Callback = std::move(Callback)](
			asio::error_code const &Error)
		{
			Accept(Service, Acceptor, std::move(Callback), RetryCount + 1);
		});
		return;
	}
	auto &ConnectionRef = *Connection;
	auto Endpoint = std::make_shared<asio::ip::tcp::endpoint>();
	auto &EndpointRef = *Endpoint;
	Acceptor.async_accept(
		ConnectionRef, 
		EndpointRef,
		[&Service, &Acceptor, Endpoint = std::move(Endpoint), Connection = std::move(Connection), Callback = std::move(Callback)](
			asio::error_code const &Error)
		{
			if (Error)
			{
				std::cerr << "Error accepting connection to " << *Endpoint << ": " 
					"(" << Error.value() << ") " << Error << std::endl;
				return;
			}
			if (Callback(std::move(Connection)))
				Accept(Service, Acceptor, std::move(Callback));
		});
}

template <typename CallbackT>
	void Read(
		std::shared_ptr<asio::ip::tcp::socket> Connection, 
		std::shared_ptr<ReadBufferT> Buffer, 
		CallbackT &&Callback)
{
	Buffer->Ensure(256);
	auto BufferStart = Buffer->EmptyStart();
	auto BufferSize = Buffer->Available();
	auto &ConnectionRef = *Connection;
	ConnectionRef.async_receive(
		asio::buffer(BufferStart, BufferSize), 
		[Connection = std::move(Connection), Buffer = std::move(Buffer), Callback = std::move(Callback)](
			asio::error_code const &Error, 
			size_t ReadSize) mutable
		{
			if (Error)
			{
				std::cerr << "Error reading: (" << Error.value() << ") " << Error << std::endl;
				return;
			}
			Buffer->Fill(ReadSize);
			if (Callback(*Buffer))
				Read(
					std::move(Connection), 
					std::move(Buffer), 
					std::move(Callback));
		});
}

int main(int argc, char **argv)
{
	try
	{
		// Configure
		if (argc < 2) throw UserErrorT() << "You must specify the mount point on the command line.";
		
		uint16_t Port;
		{
			auto EnvPort = getenv("CLUNKER_PORT");
			if (!EnvPort) 
			{
				throw UserErrorT() << "The environment variable CLUNKER_PORT must contain the desired IPC port number.";
			}
			if (!(StringT(EnvPort) >> Port)) throw UserErrorT() << "Environment variable CLUNKER_PORT has invalid port number: " << EnvPort;
		}

		struct SharedT
		{
			bool Die = false;

			std::exception_ptr ChildException;
			asio::io_service MainService;

			std::mutex Mutex;
			FilesystemT Filesystem;
			FuseT<FilesystemT> Fuse;

			SharedT(std::string const &Path) : Fuse(Path, Filesystem) {}
		} Shared(argv[1]);

		{
			struct sigaction HandlerInfo;
			memset(&HandlerInfo, 0, sizeof(struct sigaction));
			HandlerInfo.sa_handler = HandleSignal;
			sigemptyset(&(HandlerInfo.sa_mask));
			HandlerInfo.sa_flags = 0;
			sigaction(SIGINT, &HandlerInfo, NULL);
			sigaction(SIGTERM, &HandlerInfo, NULL);
			sigaction(SIGHUP, &HandlerInfo, NULL);
		}
		SignalHandlers.push_back([&Shared](void)
		{
			Shared.Die = true;
			Shared.Fuse.Kill();
			Shared.MainService.stop();
		});
		FinallyT SignalCleanup([](void)
		{
			SignalHandlers.clear();
		});

		// Start listeners on IPC thread
		asio::ip::tcp::endpoint TCPEndpoint(asio::ip::tcp::v4(), Port);
		asio::ip::tcp::acceptor TCPAcceptor(Shared.MainService, TCPEndpoint);
		Accept(Shared.MainService, TCPAcceptor, [&Shared](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([&Shared, Connection](std::shared_ptr<luxem::value> &&Data)
			{
				auto Error = [&](std::string Message)
				{
					auto Buffer = std::make_shared<std::string>(
						luxem::writer()
							.type("error")
							.value(Message)
							.dump());
					auto const &BufferArg = asio::buffer(Buffer->c_str(), Buffer->size());
					asio::async_write(
						*Connection, 
						BufferArg, 
						[Buffer = std::move(Buffer)](
							asio::error_code const &Error, 
							std::size_t WroteSize)
						{
							// noop
						});
				};

				if (!Data->has_type()) 
				{
					Error(StringT() 
						<< "Message has no type: [" << luxem::writer().value(Data).dump() << "]");
					return;
				}

				auto Type = Data->get_type();
				if (Type == "clean")
				{
					std::lock_guard<std::mutex> Guard(Shared.Mutex);
					Shared.Filesystem.Clean();
				}
				else if (Type == "set_count")
				{
					try
					{
						std::lock_guard<std::mutex> Guard(Shared.Mutex);
						Shared.Filesystem.SetCount(Data->as<luxem::primitive>().get_int());
					}
					catch (...)
					{
						Error(StringT()
							<< "Bad count [" << luxem::writer().value(Data).dump() << "]");
					}
				}
				else if (Type == "get_count")
				{
					std::lock_guard<std::mutex> Guard(Shared.Mutex);
					auto Buffer = std::make_shared<std::string>(
						luxem::writer()
							.type("count")
							.value(Shared.Filesystem.GetCount())
							.dump());
					auto const &BufferArg = asio::buffer(Buffer->c_str(), Buffer->size());
					asio::async_write(
						*Connection, 
						BufferArg, 
						[Buffer = std::move(Buffer)](
							asio::error_code const &Error, 
							std::size_t WroteSize)
						{
							// noop
						});
				}
				else
				{
					Error(StringT() <<
						"Unknown message type [" << Type << "]");
					return;
				}
			});
			Read(std::move(Connection), std::make_shared<ReadBufferT>(), [&Shared, Reader](ReadBufferT &Buffer)
			{
				try
				{
					auto Consumed = Reader->feed((char const *)Buffer.FilledStart(), Buffer.Filled(), false);
					Buffer.Consume(Consumed);
				}
				catch (UserErrorT const &Error)
				{
					std::cerr << "Error: " << Error << std::endl;
					return false;
				}
				catch (SystemErrorT const &Error)
				{
					std::cerr << "System error: " << Error << std::endl;
					return false;
				}
				catch (ConstructionErrorT const &Error)
				{
					std::cerr << "Uncaught error: " << Error << std::endl;
					return false;
				}
				catch (std::runtime_error const &Error)
				{
					std::cerr << "Uncaught error: " << Error.what() << std::endl;
					return false;
				}
				return !Shared.Die;
			});
			return !Shared.Die;
		});

		std::thread IPCThread([&Shared](void) 
		{ 
			try
			{
				Shared.MainService.run();
				std::cout << "IPC stopped " << std::endl;
			}
			catch (...)
			{
				Shared.ChildException = std::current_exception();
			}
		});

		// Start fuse on other thread
		auto Result = Shared.Fuse.Run(); 
		std::cout << "Fuse stopped " << std::endl;

		IPCThread.join();
		if (Shared.ChildException)
			std::rethrow_exception(Shared.ChildException);

		return Result;
	}
	catch (UserErrorT const &Error)
	{
		std::cerr << "Error: " << Error << std::endl;
		return 1;
	}
	catch (SystemErrorT const &Error)
	{
		std::cerr << "System error: " << Error << std::endl;
		return 1;
	}
	catch (ConstructionErrorT const &Error)
	{
		std::cerr << "Uncaught error: " << Error << std::endl;
		return 1;
	}
	catch (std::runtime_error const &Error)
	{
		std::cerr << "Uncaught error: " << Error.what() << std::endl;
		return 1;
	}
}

