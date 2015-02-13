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
		
	typedef function<void(size_t)> GetOpCountCallbackT;
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
	void SetOpCount(size_t Count, SetOpCountCallbackT &&Callback)
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
					auto Callback = std::move(Control->CleanCallbacks.front());
					Control->CleanCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "set_result")
				{
					auto Callback = std::move(Control->GetOpCountCallbacks.front());
					Control->GetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_bool());
				}
				else if (Type == "get_result")
				{
					auto Callback = std::move(Control->SetOpCountCallbacks.front());
					Control->SetOpCountCallbacks.pop_front();
					Callback(Data->as<luxem::primitive>().get_int());
				}
				else
				{
					std::cerr << "Unknown message type [" << Type << "]";
					return;
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
				Chain.Callbacks.insert(Last, std::move(Callback)));
		}
		
		friend struct CallbackChainT;
		private:
			AddT(CallbackChainT &Chain, std::list<CallbackT>::iterator Last) : 
				Chain(Chain),
				Last(Last)
			{
			}

			CallbackChainT &Chain;
			std::list<CallbackT>::iterator Last;
	};

	AddT Add(CallbackT &&Callback)
	{
		return AddT(*this, Callbacks.begin()).Add(std::move(Callback));
	}

	void Next(void)
	{
		if (Callbacks.empty()) return;
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

		static constexpr uint16_t ControlPort = 4522;
		setenv("CLUNKER_PORT", (StringT() << ControlPort).str().c_str(), 1);
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
		Root.CreateDirectory();
		SubprocessT Filesystem(
			MainService, 
			Filesystem::PathT::Qualify(argv[1]), 
			{
				Root.Render()
			});
		auto ClunkerCleanup = FinallyT([&Root, &Filesystem](void)
		{
			Filesystem.Terminate();
			Filesystem.GetResult();
			Assert(Root.DeleteDirectory());
		});
		
		// Connect to clunker
		std::shared_ptr<ClunkerControlT> Control;
		ConnectClunker(
			MainService, 
			ControlEndpoint, 
			[&Chain, &Control](std::shared_ptr<ClunkerControlT> NewControl) 
			{ 
				Control = std::move(NewControl); 
				Chain.Next(); 
			});

		// Prepare tests
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
					.Add([&Control, Callback = std::move(Callback)](void) mutable
						{ Callback(); });
			};
		};
		Chain
			.Add(WrapTest([](void) { std::cout << "ranninged test" << std::endl; })); // TODO

		// Run
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
