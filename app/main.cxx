#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include <asio.hpp>
#include <mutex>
#include <luxem-cxx/luxem.h>
#include <thread>
#include <fcntl.h>

#include "../ren-cxx-basics/error.h"
#include "../ren-cxx-basics/variant.h"
#include "../ren-cxx-filesystem/file.h"

#include "fuse_wrapper.h"
#include "asio_utils.h"

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
	FilesystemT(std::mutex &Mutex) : Mutex(Mutex), OperationCount(-1), Root(std::make_shared<FileT>())
	{
		Files.emplace(std::string("/"), Root);
		Root->stat.st_mode = 
			S_IFDIR |
			S_IRUSR | S_IWUSR | S_IXUSR |
			S_IRGRP | S_IWGRP | S_IXGRP |
			S_IROTH | S_IWOTH | S_IXOTH;
	}

	bool Clean(void) 
	{
		std::lock_guard<std::mutex> Guard(Mutex);
		Files.clear(); 
		Files.emplace(std::string("/"), Root);
		return true;
	}

	void SetCount(int64_t Count) 
	{ 
		std::lock_guard<std::mutex> Guard(Mutex);
		OperationCount = Count; 
		std::cout << "Count is now " << OperationCount << std::endl;
	}

	int64_t GetCount(void) const 
	{ 
		std::lock_guard<std::mutex> Guard(Mutex);
		return OperationCount; 
	}

	// FuseT interface
	void OperationBegin(void)
	{
		std::cout << "op" << std::endl;
		Mutex.lock();
	}

	void OperationEnd(void)
	{
		Mutex.unlock();
	}

#define OPER if (!DecrementCount()) return -EIO;

	int getattr(const char *path, struct stat *buf)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		*buf = Found->second->stat;
		return 0;
	}

	int opendir(const char *path, struct fuse_file_info *fi)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!CheckPermission(
			*Found->second,
			(fi->flags == O_RDONLY) || (fi->flags == O_RDWR),
			(fi->flags == O_WRONLY) || (fi->flags == O_RDWR),
			false)) return -EACCES;
		if (Found->second->Data) return -ENOTDIR;
		return 0;
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
			if (filler(buf, Filename.c_str(), &Test->second->stat, Count)) break;
		}
		return 0;
	}

	int mkdir(const char *path, mode_t mode)
	{
		OPER
		auto Root = Files.emplace(std::string(path), std::make_shared<FileT>()).first;
		Root->second->stat.st_mode = 
			mode |
			S_IFDIR;
		return 0;
	}

	int rmdir(const char *path)
	{
		OPER
		std::string Path(path);
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (Found->second->Data) return -ENOTDIR;
		auto Next = Found;
		++Next;
		if (InDir(Next, Path)) return -ENOTEMPTY;
		Files.erase(Found);
		return 0;
	}

	int create(const char *path, mode_t mode, struct fuse_file_info *fi)
	{
		OPER
		auto Root = Files.emplace(std::string(path), std::make_shared<FileT>()).first;
		Root->second->stat.st_mode = mode;
		Root->second->stat.st_mode = 
			mode |
			S_IFREG;
		Root->second->Data = std::vector<uint8_t>();
		SetFile(fi, Root->second);
		return 0;
	}
	
	int release(const char *path, struct fuse_file_info *fi)
	{
		ClearFile(fi);
		return 0;
	}

	int utimens(const char *path, const struct timespec tv[2])
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		auto &stat = Found->second->stat;
		stat.st_atim = tv[0];
		stat.st_mtim = tv[1];
		return 0;
	}

	int access(const char *path, int amode)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (amode == F_OK) return 0;
		if (!CheckPermission(
			*Found->second, 
			amode & R_OK,
			amode & W_OK,
			amode & X_OK)) return -EACCES;
		return 0;
	}

	int unlink(const char *path)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!Found->second->Data) return -EPERM;
		Files.erase(Found);
		return 0;
	}

	int open(const char *path, struct fuse_file_info *fi)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		if (!CheckPermission(
			*Found->second,
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
		auto &File = *Found->second;
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
		Found->second->stat.st_mode = mode;
		return 0;
	}

	int chown(const char *path, uid_t uid, gid_t gid)
	{
		OPER
		auto Found = Files.find(path);
		if (Found == Files.end()) return -ENOENT;
		Found->second->stat.st_uid = uid;
		Found->second->stat.st_gid = gid;
		return 0;
	}
		
	friend struct FuseT<FilesystemT>;
	private:
		// Utility methods
		bool DecrementCount(void)
		{
			if (OperationCount < 0) return true;
			if (OperationCount == 0) 
			{
				std::cout << "Op failed!" << std::endl;
				return false;
			}
			OperationCount -= 1;
			return true;
		}
	
		template <typename IteratorT>
			bool InDir(IteratorT const &Test, std::string const &Path)
		{
			return
				(Test != Files.end()) &&
				(Test->first.size() > Path.size()) &&
				(Test->first.substr(0, Path.size()) == Path);
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

		void SetFile(struct fuse_file_info *fi, std::shared_ptr<FileT> File)
		{
			fi->fh = reinterpret_cast<uint64_t>(new std::shared_ptr<FileT>(std::move(File)));
		}

		FileT &GetFile(struct fuse_file_info *fi)
		{
			return **reinterpret_cast<std::shared_ptr<FileT> *>(fi->fh);
		}

		void ClearFile(struct fuse_file_info *fi)
		{
			delete reinterpret_cast<std::shared_ptr<FileT> *>(fi->fh);
		}

		std::mutex &Mutex;
		int64_t OperationCount;

		std::shared_ptr<FileT> Root;

		std::map<std::string, std::shared_ptr<FileT>> Files;
};

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

			asio::io_service MainService;

			std::mutex Mutex;
			FilesystemT Filesystem;
			FuseT<FilesystemT> Fuse;

			SharedT(std::string const &Path) : 
				Filesystem(Mutex), 
				Fuse(Path, Filesystem) {}
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
		TCPListen(Shared.MainService, TCPEndpoint, [&Shared](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([&Shared, Connection](std::shared_ptr<luxem::value> &&Data)
			{
				auto Error = [&](std::string Message)
				{
					Write(Connection,
						luxem::writer()
							.type("error")
							.value(Message)
							.dump());
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
					auto Success = Shared.Filesystem.Clean();
					Write(Connection, 
						luxem::writer()
							.type("clean_result")
							.value(Success)
							.dump());
				}
				else if (Type == "set_count")
				{
					bool Success = false;
					try
					{
						Shared.Filesystem.SetCount(Data->as<luxem::primitive>().get_int());
						Success = true;
					}
					catch (...)
					{
						Error(StringT()
							<< "Bad count [" << luxem::writer().value(Data).dump() << "]");
						Success = false;
					}
					Write(Connection, 
						luxem::writer()
							.type("set_result")
							.value(Success)
							.dump());
				}
				else if (Type == "get_count")
				{
					Write(Connection,
						luxem::writer()
							.type("count")
							.value(Shared.Filesystem.GetCount())
							.dump());
				}
				else
				{
					Error(StringT() <<
						"Unknown message type [" << Type << "]");
					return;
				}
			});
			LoopRead(std::move(Connection), [&Shared, Reader](ReadBufferT &Buffer)
			{
				auto Consumed = Reader->feed(
					(char const *)Buffer.FilledStart(), 
					Buffer.Filled(), 
					false);
				Buffer.Consume(Consumed);
				return !Shared.Die;
			});
			return !Shared.Die;
		});

		std::thread IPCThread([&Shared](void) 
		{ 
			Shared.MainService.run();
			std::cout << "IPC stopped " << std::endl;
		});

		// Start fuse on other thread
		auto Result = Shared.Fuse.Run(); 
		std::cout << "Fuse stopped " << std::endl;

		IPCThread.join();

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

