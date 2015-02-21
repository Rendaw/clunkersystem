#ifndef fuse_outofband_h
#define fuse_outofband_h

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fuse_lowlevel.h>

// OOBFilesystem : Filesystem : OOBControl

struct OutOfBandControlT
{
	protected:
		OutOfBandControlT(void) : Root(true) {}

		void IBCreate(std::string const &Path, bool Directory)
		{
			CTCreate(Path.c_str(), Directory);
		}

		bool OOBRemoveFile(std::string const &Path)
		{
			//std::cout << "ib -> unlink start" << std::endl;
			auto Result = ::unlink(Path.c_str());
			//std::cout << "ib -> unlink end" << std::endl;
			if (Result != 0) 
			{
				std::cerr << "Out of bound unlink failed: " << strerror(errno) << std::endl;
				return false;
			}
			return true;
		}
		
		bool OOBRemoveDir(std::string const &Path)
		{
			//std::cout << "ib -> rmdir start" << std::endl;
			auto Result = ::rmdir(Path.c_str());
			//std::cout << "ib -> rmdir end" << std::endl;
			if (Result != 0) 
			{
				std::cerr << "Out of bound rmdir failed: " << strerror(errno) << std::endl;
				return false;
			}
			return true;
		}

		void IBRemove(std::string const &Path)
		{
			CTDestroy(Path.c_str());
		}

		void IBRename(std::string const &From, std::string const &To)
		{
			CTMove(From.c_str(), To.c_str());
		}
		
		void IBLink(std::string const &From, std::string const &To)
		{
			CTLink(From.c_str(), To.c_str());
		}
		
		//
		void CTCreate(char const *Path, bool Directory)
		{
			std::cout << "+ " << Path << std::endl;
			std::lock_guard<std::mutex> Guard(this->Mutex);
			Root.Find(Path + 1, true, Directory);
		}
		
		void CTDestroy(char const *Path)
		{
			std::cout << " - " << Path << std::endl;
			std::lock_guard<std::mutex> Guard(this->Mutex);
			Root.Destroy(Path + 1);
		}

		bool CTIsDir(char const *Path)
		{
			std::lock_guard<std::mutex> Guard(this->Mutex);
			auto &Found = Root.Find(Path + 1, false, false);
			return Found->Children;
		}

		void CTLink(char const *From, char const *To)
		{
			std::cout << "+ " << To << std::endl;
			std::lock_guard<std::mutex> Guard(this->Mutex);
			Root.Link(From + 1, To + 1);
		}

		void CTMove(char const *From, char const *To)
		{
			std::cout << " - " << From << std::endl;
			std::cout << "+ " << To << std::endl;
			std::lock_guard<std::mutex> Guard(this->Mutex);
			Root.Move(From + 1, To + 1);
		}

	private:
		std::mutex Mutex;
		struct CacheTreeT
		{
			CacheTreeT(bool Directory)
			{
				if (Directory) Children = std::map<std::string, std::shared_ptr<CacheTreeT>>();
			}

			inline std::shared_ptr<CacheTreeT> &Find(char const *Path, bool const Create, bool const Directory)
			{
				size_t Length = 0;
				while (true)
				{
					if (Path[Length] == 0) break;
					if (Path[Length] == '/')
					{
						auto Found = Children->find(std::string(Path, Length));
						Assert(Found != Children->end());
						return Found->second->Find(Path + Length + 1, Create, Directory);
					}
					++Length;
				}
				AssertGT(Length, 0u);
				auto Placed = Children->emplace(
					std::string(Path, Length), 
					std::make_shared<CacheTreeT>(Directory));
				AssertE(Create, Placed.second);
				return Placed.first->second;
			}

			inline void Destroy(char const *Path)
			{
				size_t Length = 0;
				while (true)
				{
					if (Path[Length] == 0) break;
					if (Path[Length] == '/')
					{
						auto Found = Children->find(std::string(Path, Length));
						Assert(Found != Children->end());
						Found->second->Destroy(Path + Length + 1);
						return;
					}
					++Length;
				}
				AssertGT(Length, 0u);
				Children->erase(std::string(Path, Length));
			}

			void Move(char const *From, char const *To)
			{
				auto &Found = Find(From, false, false);
				auto &Created = Find(To, true, Found->Children);
				Created = std::move(Found);
				Destroy(From);
			}
			
			void Link(char const *From, char const *To)
			{
				auto &Found = Find(From, false, false);
				auto &Created = Find(To, true, Found->Children);
				Created = Found;
			}

			OptionalT<std::map<std::string, std::shared_ptr<CacheTreeT>>> Children;
		} Root;
};

template <typename FilesystemT> struct OutOfBandFilesystemT : FilesystemT
{
	template <typename ...ArgsT> OutOfBandFilesystemT(ArgsT &&...Args) : 
		FilesystemT(std::forward<ArgsT &&>(Args)...) {}

	void OperationBegin(bool const OutOfBand)
	{
		if (OutOfBand) return;
		FilesystemT::OperationBegin(OutOfBand);
	}

	void OperationEnd(bool const OutOfBand)
	{
		if (OutOfBand) return;
		FilesystemT::OperationEnd(OutOfBand);
	}

	int getattr(bool const OutOfBand, const char *path, struct stat *buf)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB getattr: " << path << std::endl;
			memset(buf, 0, sizeof(*buf));
			buf->st_mode = 
				S_IRUSR | S_IWUSR | S_IXUSR |
				S_IRGRP | S_IWGRP | S_IXGRP |
				S_IROTH | S_IWOTH | S_IXOTH;
			bool IsDir = this->CTIsDir(path);
			if (IsDir) buf->st_mode |= S_IFDIR;
			else buf->st_mode |= S_IFREG;
			return 0;
		}
		else return FilesystemT::getattr(OutOfBand, path, buf);
	}

	int mkdir(bool const OutOfBand, const char *path, mode_t mode)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB mkdir: " << path << std::endl;
			this->CTCreate(path, true);
			return 0;
		}
		else return FilesystemT::mkdir(OutOfBand, path, mode);
	}
	
	int rmdir(bool const OutOfBand, const char *path)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB rmdir: " << path << std::endl;
			this->CTDestroy(path);
			return 0;
		}
		else return FilesystemT::rmdir(OutOfBand, path);
	}
	
	int create(bool const OutOfBand, const char *path, mode_t mode, struct fuse_file_info *fi)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB create: " << path << std::endl;
			this->CTCreate(path, false);
			return 0;
		}
		else return FilesystemT::create(OutOfBand, path, mode, fi);
	}
	
	int unlink(bool const OutOfBand, const char *path)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB unlink: " << path << std::endl;
			this->CTDestroy(path);
			return 0;
		}
		else return FilesystemT::unlink(OutOfBand, path);
	}
	
	int rename(bool const OutOfBand, const char *from, const char *to)
	{
		if (OutOfBand)
		{
			//std::cout << "OOB rename: " << from << " " << to << std::endl;
			this->CTMove(from, to);
			return 0;
		}
		else return FilesystemT::rename(OutOfBand, from, to);
	}
};

#endif

