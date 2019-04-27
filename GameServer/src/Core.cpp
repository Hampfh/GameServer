#include "pch.h"
#include "Core.h"

//https://www.ibm.com/support/knowledgecenter/en/ssw_ibm_i_72/rzab6/xnonblock.htm

Core::Core() {
	clientIndex_ = 0;
	maxConnections_ = 0;
	seed_ = 0;
	listening_ = socket(NULL, NULL, NULL);

	// Initialization
	sharedMemory_ = new SharedMemory;

	// Setup core logger
	std::vector<spdlog::sink_ptr> sinks;
	sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
	sinks.push_back(sharedMemory_->GetFileSink());
	log_ = std::make_shared<spdlog::logger>("Core", begin(sinks), end(sinks));
	log_->set_pattern("[%a %b %d %H:%M:%S %Y] [%L] %^%n: %v%$");
	register_logger(log_);

	SetupConfig();

	SetupWinSock();

	// Create main lobby
	sharedMemory_->CreateMainLobby();

	running_ = true;

}

Core::~Core() {
	
}

void Core::CleanUp() const {
	// Clean up server
	WSACleanup();

	delete sharedMemory_;

}

void Core::SetupConfig() {
	log_->info("Loading configuration file...");
	try {
		// Load content from conf file
		scl::config_file file("server.conf", scl::config_file::READ);

		for (std::pair<std::string, std::string> setting : file) {
			std::string& selector = setting.first;
			std::string& value = setting.second;

			if (selector == "server_port") {
				port_ = std::stoi(value);
			}
			else if (selector == "clock_speed") {
				sharedMemory_->SetClockSpeed(std::stoi(value));
			}
			else if(selector == "socket_processing_max") {
				timeInterval_.tv_usec = std::stoi(value) * 1000;
			}
			else if (selector == "timeout_tries") {
				sharedMemory_->SetTimeoutTries(std::stoi(value));
			}
			else if (selector == "timeout_delay") {
				sharedMemory_->SetTimeoutDelay(std::stof(value));
			}
			else if (selector == "lobby_max_connections") {
				sharedMemory_->SetLobbyMax(std::stoi(value));
			}
			else if (selector == "start_id_at") {
				clientIndex_ = std::stoi(value);
			}
			else if (selector == "max_connections") {
				maxConnections_ = std::stoi(value);
			} 
			else if (selector == "lobby_start_id_at") {
				sharedMemory_->SetLobbyStartId(std::stoi(value));
			}
		}
		log_->info("Configurations loaded!");
	} catch (scl::could_not_open_error&) {
		log_->warn("No configuration file found, creating conf file...");

		// Create standard values

		scl::config_file file("server.conf", scl::config_file::WRITE);
		
		// Generating settings
		file.put(scl::comment(" Server settings"));
		file.put(scl::comment(" (All settings associated with time are defined in milliseconds)"));
		file.put("server_port", 15000);
		file.put("clock_speed", 50);
		file.put("socket_processing_max", 1);
		file.put("timeout_tries", 30);
		file.put("timeout_delay", 0.5);
		file.put("max_connections", 10);

    file.put(scl::comment(" Lobby settings"));
		file.put("lobby_max_connections", 5);
		file.put("lobby_start_id_at", 1);
		file.put(scl::comment(" Client settings"));;
		file.put("start_id_at", 0);

		// Create file
		file.write_changes();
		//and close it to save memory.
		file.close();

		log_->info("Configuration file created!");

		SetupConfig();
	}
}

void Core::SetupWinSock() {

	// Initialize win sock	
	const WORD ver = MAKEWORD(2, 2);
	WSADATA wsaData;
	const int wsOk = WSAStartup(ver, &wsaData);
	if (wsOk != 0) {
		log_->error("Can't initialize winsock2");
		return;
	}

	// Create listening socket
	listening_ = socket(AF_INET, SOCK_STREAM, 0);
	if (listening_ == INVALID_SOCKET) {
		log_->error("Can't create listening socket");
		return;
	}

	// Binding connections
	sockaddr_in hint = sockaddr_in();
	hint.sin_family = AF_INET;
	hint.sin_port = htons(port_);
	hint.sin_addr.S_un.S_addr = INADDR_ANY;

	// Bind connections
	bind(listening_, reinterpret_cast<const sockaddr*>(&hint), sizeof(hint));

	// Add listening socket
	listen(listening_, SOMAXCONN);

	// Create socket array
	fd_set master;
	FD_ZERO(&master);
	workingSet_ = master;

	// Add listening socket to array
	FD_SET(listening_, &master);

	// Generate server seed
	std::random_device rd;
	std::default_random_engine gen(rd());
	seed_ = gen();

	log_->info("Server seed is " + std::to_string(seed_));

	log_->info("Server port is " + std::to_string(port_));

	// Assign
	sharedMemory_->SetSockets(master);
}

void Core::Execute() {

	while(running_) {
		Loop();
	}
	CleanUp();
}

void Core::Loop() {

	// Duplicate master
	workingSet_ = *sharedMemory_->GetSockets();

	// Get socket list size
	const int result = select(0, &workingSet_, nullptr, nullptr, &timeInterval_);

	// Add new connection to main lobby
	InitializeReceiving(result);
	

	// Default sleep time between responses
	std::this_thread::sleep_for(std::chrono::milliseconds(sharedMemory_->GetClockSpeed()));
}

void Core::InitializeReceiving(const int select_result) {

	for (int i = 0; i < select_result; i++) {

		const SOCKET socket = workingSet_.fd_array[i];

		if (socket == listening_) {

			// Check for new connections
			const SOCKET newClient = accept(listening_, nullptr, nullptr);

			// Check if max clients is reached
			if (maxConnections_ <= sharedMemory_->GetConnectedClients()) {
				// Immediately close socket if max connections is reached
				closesocket(newClient);
				break;
			}

			sharedMemory_->AddSocket(newClient);

			// Add newClient to socketContentList
			clientIndex_++;

			// Create and connect it to main lobby
			auto* clientObject = new Client(newClient, sharedMemory_, clientIndex_, sharedMemory_->GetMainLobby()->id);

			// Create a setup message
			std::string welcomeMsg = "Successfully connected to server|" + std::to_string(clientIndex_) + "|" + std::to_string(seed_);

			// Send the message to the new client
			send(newClient, welcomeMsg.c_str(), static_cast<int>(welcomeMsg.size()) + 1, 0);

			// Console message
			log_->info("Client#" + std::to_string(newClient) + " connected to the server");

			//send(newClient, std::to_string(clientIndex_).c_str(), static_cast<int>(std::to_string(clientIndex_).size()) + 1, 0);

			// wait a little bit before sending the next message
			std::this_thread::sleep_for(std::chrono::microseconds(10));

			//send(newClient, std::to_string(seed_).c_str(), static_cast<int>(std::to_string(seed_).size()) + 1, 0);
			// Console message
			log_->info("Client#" + std::to_string(newClient) + " was assigned ID " + std::to_string(clientIndex_));

			// Connect client to main lobby
			sharedMemory_->GetMainLobby()->AddClient(clientObject, false);

			// Connect the new client to a new thread
			std::thread clientThread(&Client::Loop, clientObject);
			clientThread.detach();
			break;
		}
	}
}

void Core::BroadcastCoreCall(int lobby, int receiver, int command) const {
	Lobby* current = sharedMemory_->GetFirstLobby();

	while (current != nullptr) {
		current->BroadcastCoreCall(lobby, receiver, command);
		current = current->next;
	}
}

void Core::Interpreter() {
	std::string command;
	while (true) {

		std::vector<std::string> part;

		std::getline(std::cin, command);
		
		const std::regex regexCommand("[^ ]+");
		std::smatch matcher;

		// Check if input is a command call
		if (command[0] != '/') {
			continue;
		}

		while (std::regex_search(command, matcher, regexCommand)) {
			for (auto addSegment : matcher) {
				// Append the commands to the list
				part.push_back(addSegment.str());
			}
			command = matcher.suffix().str();
		}

		if (!part.empty()) {
			if (part.size() >= 3 && part[0] == "/Client" && sharedMemory_->IsInt(part[1]) && part[2] == "drop") {
				// The first selector is lobby and the second is for client
				Lobby* clientLobby = nullptr;
				Client* currentClient = sharedMemory_->FindClient(std::stoi(part[1]), &clientLobby);

				if (currentClient != nullptr && clientLobby != nullptr) {
					clientLobby->DropClient(currentClient);
				} else {
					log_->warn("Client or lobby not found");
				}
				
			} else if (part[0] == "/Lobby") {
				if (part.size() >= 2 && part[1] == "create") {
					Lobby* newLobby = sharedMemory_->AddLobby();
					log_->info("Lobby created with id: " + std::to_string(newLobby->id));
				}
				else if (part.size() >= 3 && sharedMemory_->IsInt(part[1]) && part[2] == "start") {
					BroadcastCoreCall(std::stoi(part[1]), 0, Command::start);
					log_->info("Lobby started!");
				}
				else if (part.size() >= 3 && sharedMemory_->IsInt(part[1]) && part[2] == "pause") {
					BroadcastCoreCall(std::stoi(part[1]), 0, Command::pause);
					log_->info("Lobby paused!");
				}
				else if (part.size() >= 3 && sharedMemory_->IsInt(part[1]) && part[2] == "drop") {

					// Can't drop main lobby
					if (std::stoi(part[1]) != sharedMemory_->GetMainLobby()->id) {
						Lobby* current = sharedMemory_->GetFirstLobby();
						// Search for the lobby with the assigned id
						while (current != nullptr) {
							if (current->id == std::stoi(part[1])) {
								sharedMemory_->DropLobby(std::stoi(part[1]));
								break;
							}
							current = current->next;
						}
					} else {
						log_->warn("Can't drop main lobby");
					}
					continue;
				}
				// Second argument is lobby, third is client
				else if (part.size() >= 4 && sharedMemory_->IsInt(part[1]) && part[2] == "summon" && sharedMemory_->IsInt(part[3])) {
					const int newLobbyId = std::stoi(part[1]);
					const int clientId = std::stoi(part[3]);

					// Find the current lobby and client
					Lobby* currentLobby = nullptr;
					Lobby* targetLobby = sharedMemory_->GetLobby(newLobbyId);
					Client* currentClient = sharedMemory_->FindClient(clientId, &currentLobby);

					if (currentClient != nullptr) {
						if (targetLobby != nullptr) {
							currentLobby->DropClient(clientId, true);

							// Move client to the selected lobby
							targetLobby->AddClient(currentClient);

						} else {
							log_->warn("Lobby#" + std::to_string(clientId) + " not found");
						}
					}
					else {
						log_->warn("Client not found");
						continue;
					}
				}
			}
			else if (part[0] == "/Stop") {
				running_ = false;
			}
		}
		else log_->warn("No such command");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}