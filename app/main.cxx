#define BOOST_ASIO_ENABLE_HANDLER_TRACKING

#include <asio.hpp>
#include <mutex>
#include <luxem-cxx/luxem.h>
#include <chrono>

#include "../ren-cxx-basics/error.h"
#include "../ren-cxx-basics/variant.h"
#include "../ren-cxx-filesystem/file.h"

struct FilesystemT
{
	FilesystemT(void) : Count(0) {}
	void Clean(void) {}
	void SetCount(size_t Count) { this->Count = Count; }
	size_t GetCount(void) const { return Count; }

	private:
		size_t Count;
};

template <typename CallbackT>
	void Accept(
		asio::io_service &Service, 
		asio::ip::tcp::acceptor &Acceptor, 
		CallbackT &&Callback,
		size_t RetryCount = 0)
{
	std::cout << "accepting" << std::endl;
	std::shared_ptr<asio::ip::tcp::socket> Connection;
	try
	{
		Connection = std::make_shared<asio::ip::tcp::socket>(Service);
	}
	catch (...)
	{
		std::cout << "accept error" << std::endl;
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

int main(void)
{
	try
	{
		// Configure
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

			std::mutex Mutex;
			//fuse_session *FuseSession = nullptr;
			OptionalT<size_t> Remaining;
			FilesystemT Filesystem;
		};
		auto Shared = std::make_shared<SharedT>();

		// Start fuse on other thread
		/*FuseT<FilesystemT> Fuse;
		std::thread FuseThread([Shared](void)
		{
			RunFuse(
				[Shared](fuse_session *Session) 
				{ 
					std::lock_guard<std::mutex> Guard(Shared->Mutex);
					Shared->FuseSession = Session; 
				},
				&Shared->Filesystem);
		});*/

		// Start listeners on main thread
		asio::io_service MainService;

		asio::signal_set SignalTask(MainService, SIGINT, SIGTERM);
		SignalTask.async_wait([Shared](asio::error_code const &Error, int SignalNumber)
		{
			Shared->Die = true;
			std::lock_guard<std::mutex> Guard(Shared->Mutex);
			/*if (Shared->FuseSession)
				fuse_session_exit(Shared->FuseSession);*/
		});

		asio::ip::tcp::endpoint TCPEndpoint(asio::ip::tcp::v4(), Port);
		asio::ip::tcp::acceptor TCPAcceptor(MainService, TCPEndpoint);
		Accept(MainService, TCPAcceptor, [Shared](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([Shared, Connection](std::shared_ptr<luxem::value> &&Data)
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
					std::lock_guard<std::mutex> Guard(Shared->Mutex);
					Shared->Filesystem.Clean();
				}
				else if (Type == "set_count")
				{
					try
					{
						std::lock_guard<std::mutex> Guard(Shared->Mutex);
						Shared->Filesystem.SetCount(Data->as<luxem::primitive>().get_int());
					}
					catch (...)
					{
						Error(StringT()
							<< "Bad count [" << luxem::writer().value(Data).dump() << "]");
					}
				}
				else if (Type == "get_count")
				{
					std::lock_guard<std::mutex> Guard(Shared->Mutex);
					auto Buffer = std::make_shared<std::string>(
						luxem::writer()
							.type("count")
							.value(Shared->Filesystem.GetCount())
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
			Read(std::move(Connection), std::make_shared<ReadBufferT>(), [Shared, Reader](ReadBufferT &Buffer)
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
				return !Shared->Die;
			});
			return !Shared->Die;
		});

		MainService.run();

		//FuseThread.join();
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

	return 0;
}
