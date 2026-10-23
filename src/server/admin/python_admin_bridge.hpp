/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

class Logger;

class PythonAdminBridge {
public:
	explicit PythonAdminBridge(Logger &logger);
	~PythonAdminBridge();

	PythonAdminBridge(const PythonAdminBridge &) = delete;
	PythonAdminBridge &operator=(const PythonAdminBridge &) = delete;

	void start();
	void stop();

	bool isEnabled() const {
		return enabled;
	}

	uint16_t getPort() const {
		return port;
	}

private:
	void runLoop();
	std::string handleRequest(const std::string &requestText) const;
	std::string buildStatusJson() const;
	std::string executeCommand(const std::string &command) const;

	Logger &logger;
	std::atomic_bool running = false;
	bool enabled = true;
	uint16_t port = 18991;
	std::thread worker;
	std::unique_ptr<asio::io_context> ioContext;
	std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
};
