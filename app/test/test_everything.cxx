#include "../asio_utils.h"

#include <luxem-cxx/luxem.h>

#include "../../ren-cxx-basics/variant.h"
#include "../../ren-cxx-basics/function.h"
#include "../../ren-cxx-basics/error.h"
#include "subprocess.h"

struct ClunkerControlT
{
	typedef function<void(bool Success)> CleanCallbackT;
	void Clean(CleanCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("clean")
				.value("")
				.dump());
		CleanCallbacks.emplace_back(std::move(Callback));
	}
		
	typedef function<void(int64_t)> GetOpCountCallbackT;
	void GetOpCount(GetOpCountCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("get_count")
				.value("")
				.dump());
		GetOpCountCallbacks.push_back(std::move(Callback));
	}

	typedef function<void(bool Success)> SetOpCountCallbackT;
	void SetOpCount(int64_t Count, SetOpCountCallbackT &&Callback)
	{
		Write(Connection, 
			luxem::writer()
				.type("set_count")
				.value(Count)
				.dump());
		SetOpCountCallbacks.push_back(std::move(Callback));
	}

	friend void ConnectClunker(
		asio::io_service &Service, 
		asio::ip::tcp::endpoint &Endpoint, 
		function<void(std::shared_ptr<ClunkerControlT> Control)> &&Callback);
	private:
		std::shared_ptr<asio::ip::tcp::socket> Connection;
				
		std::list<CleanCallbackT> CleanCallbacks;
		std::list<GetOpCountCallbackT> GetOpCountCallbacks;
		std::list<SetOpCountCallbackT> SetOpCountCallbacks;
};

void ConnectClunker(
	asio::io_service &Service, 
	asio::ip::tcp::endpoint &Endpoint, 
	function<void(std::shared_ptr<ClunkerControlT> Control)> &&Callback)
{
	TCPConnect(
		Service, 
		Endpoint, 
		[Callback = std::move(Callback)](std::shared_ptr<asio::ip::tcp::socket> Connection)
		{
			auto Control = std::make_shared<ClunkerControlT>();
			Control->Connection = Connection;

			auto Reader = std::make_shared<luxem::reader>();
			Reader->element([Control, Connection](std::shared_ptr<luxem::value> &&Data)
			{
				if (!Data->has_type()) 
				{
					std::cerr << "Message has no type: [" << luxem::writer().value(Data).dump() << "]";
					return;
				}

				auto Type = Data->get_type();
				if (Type == "clean_result")
				{
					AssertGT(Control->CleanCallbacks.size(), 0u);
					auto Callback = std::move(Control->CleanCallbacks.front());
					Control->CleanCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "set_result")
				{
					AssertGT(Control->SetOpCountCallbacks.size(), 0u);
					auto Callback = std::move(Control->SetOpCountCallbacks.front());
					Control->SetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "count")
				{
					AssertGT(Control->GetOpCountCallbacks.size(), 0u);
					auto Callback = std::move(Control->GetOpCountCallbacks.front());
					Control->GetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_int());
				}
				else
				{
					throw SystemErrorT() << "Unknown message type [" << Type << "]";
				}
			});

			LoopRead(std::move(Connection), [Reader](ReadBufferT &Buffer)
			{
				auto Consumed = Reader->feed((char const *)Buffer.FilledStart(), Buffer.Filled(), false);
				Buffer.Consume(Consumed);
				return true;
			});

			Callback(std::move(Control));
		});
}

struct CallbackChainT
{
	typedef function<void(void)> CallbackT;
	struct AddT
	{
		AddT Add(CallbackT &&Callback)
		{
			return AddT(
				Chain, 
				++Chain.Callbacks.insert(Next, std::move(Callback)));
		}
		
		friend struct CallbackChainT;
		private:
			AddT(CallbackChainT &Chain, std::list<CallbackT>::iterator Next) : 
				Chain(Chain),
				Next(Next)
			{
			}

			CallbackChainT &Chain;
			std::list<CallbackT>::iterator Next;
	};

	AddT Add(CallbackT &&Callback)
	{
		return AddT(*this, Callbacks.begin()).Add(std::move(Callback));
	}

	void Next(void)
	{
		if (Callbacks.empty()) 
		{
			std::cout << "No chain callbacks, next does nothing." << std::endl;
			return;
		}
		auto Callback = std::move(Callbacks.front());
		Callbacks.pop_front();
		Callback();
	}

	private:
		std::list<CallbackT> Callbacks;
};

int main(int argc, char **argv)
{
	try
	{
		// Check arguments, prepare config
		if (argc < 2) throw UserErrorT() << "Missing clunkersystem executable argument.";

		uint16_t ControlPort = 0;
		if (!getenv("CLUNKER_PORT")) throw UserErrorT() << "CLUNKER_PORT env variable is not set.";
		StringT(getenv("CLUNKER_PORT")) >> ControlPort;
		asio::ip::tcp::endpoint ControlEndpoint(
			asio::ip::address_v4::loopback(),
			ControlPort);

		// Test call chain
		CallbackChainT Chain;
		
		// Start signal listening
		asio::io_service MainService;

		asio::signal_set Signals(MainService, SIGTERM, SIGINT, SIGHUP);
		Signals.async_wait([&MainService](asio::error_code const &Error, int SignalNumber)
		{
			MainService.stop();
		});

		// Start clunker
		auto Root = Filesystem::PathT::Qualify("test_root");
		//Root.GoTo();
		Root.CreateDirectory();
		SubprocessT FilesystemProcess(
			MainService, 
			Filesystem::PathT::Qualify(argv[1]), 
			{
				Root.Render()
			});
		auto ClunkerCleanup = FinallyT([&Root, &FilesystemProcess](void)
		{
			FilesystemProcess.Terminate();
			FilesystemProcess.GetResult();
			Assert(Root.DeleteDirectory());
		});
		Root.GoTo();
		
		// Connect to clunker
		std::shared_ptr<ClunkerControlT> Control;
		ConnectClunker(
			MainService, 
			ControlEndpoint, 
			[&Chain, &Control](std::shared_ptr<ClunkerControlT> NewControl) 
			{ 
				std::cout << "Got connection, starting chain." << std::endl;
				Control = std::move(NewControl); 
				Chain.Next(); 
			});

		// Prepare tests
		Chain.Add([&MainService](void) 
		{ 
			std::cout << "Tests completed successfully." << std::endl;
			MainService.stop(); 
		});

		auto WrapTest = [&Chain, &Control](CallbackChainT::CallbackT &&Callback) mutable
		{
			return [&Chain, &Control, Callback = std::move(Callback)](void) mutable
			{
				Chain
					.Add([&Control, &Chain](void) mutable
					{
						Control->Clean([&Chain](bool Success)
						{
							if (!Success) throw SystemErrorT() << "Clean failed - test case may be broken.";
							Chain.Next();
						});
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Callback = std::move(Callback)](void) mutable
					{ 
						Callback(); 
					});
				Chain.Next();
			};
		};
		size_t TestIndex = 1;
		Chain
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Noop test" << std::endl; 
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test clearing" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("roast beef");
				Filesystem::FileT::OpenWrite(Path).Write("logos");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->Clean([&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						try
						{
							Filesystem::FileT::OpenRead(Path);
							Assert(false);
						}
						catch (ConstructionErrorT const &Error) {}
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test op count reset" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("roast beef");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(0, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						try
						{
							Filesystem::FileT::OpenWrite(Path).Write("logos");
							Assert(false);
						}
						catch (ConstructionErrorT const &Error) {}
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						try
						{
							Filesystem::FileT::OpenWrite(Path).Write("logos");
						}
						catch (ConstructionErrorT const &Error) 
						{
							Assert(false);
						}
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test op count decrement" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("plaster");
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(2000, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path](void)
					{
						Filesystem::FileT::OpenWrite(Path).Write("logos");
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->GetOpCount([&Chain](int64_t Count) 
						{ 
							AssertLT(Count, 2000);
							Chain.Next(); 
						});
					})
					;
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain](void) 
			{ 
				std::cout << TestIndex++ << " Test various file ops" << std::endl; 
				// TODO
				// Create file
				// Create dir
				// Create file in dir
				// Write to file in dir
				// Write to file in dir
				// Create subdir
				// ls dir
				// ls root
				// rm nonempty dir
				// mv file, original doesn't exist
				// rm file, file doesn't exist
				// create new file, empty
				Chain.Next();
			}))
			.Add(WrapTest([&TestIndex, &Chain, &Control](void) 
			{ 
				std::cout << TestIndex++ << " Test scheduled clunk" << std::endl; 
				auto Path = Filesystem::PathT::Qualify("chicken");
				auto LastWritten = std::make_shared<std::string>();
				Chain
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(200, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path, LastWritten](void)
					{
						for (size_t Count = 0; Count < 1000; ++Count)
						{
							try
							{
								*LastWritten = StringT() << Count;
								auto File = Filesystem::FileT::OpenWrite(Path);
								File.Write(*LastWritten);
							}
							catch (...) {}
						}
						Chain.Next();
					})
					.Add([&Control, &Chain](void)
					{
						Control->SetOpCount(-1, [&Chain](bool Success) { Chain.Next(); });
					})
					.Add([&Control, &Chain, Path, LastWritten](void)
					{
						auto Buffer = Filesystem::FileT::OpenRead(Path).ReadAll();
						AssertNE(
							std::string((char const *)&Buffer[0], Buffer.size()),
							*LastWritten);
						Chain.Next();
					})
					;
				Chain.Next();
			}))
			;

		MainService.run();
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
