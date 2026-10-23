/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/admin/python_admin_bridge.hpp"

#include <cstdlib>
#include <future>
#include <sstream>

#include "game/functions/game_reload.hpp"
#include "game/game.hpp"
#include "game/game_definitions.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "game/scheduling/save_manager.hpp"
#include "lib/logging/logger.hpp"

namespace {

std::string escapeJson(const std::string &value) {
	std::string escaped;
	escaped.reserve(value.size() + 8);
	for (const char ch : value) {
		switch (ch) {
			case '\\':
			case '"':
				escaped.push_back('\\');
				escaped.push_back(ch);
				break;
			case '\n':
				escaped += "\\n";
				break;
			case '\r':
				escaped += "\\r";
				break;
			case '\t':
				escaped += "\\t";
				break;
			default:
				escaped.push_back(ch);
				break;
		}
	}
	return escaped;
}

std::string gameStateToString(const GameState_t state) {
	switch (state) {
		case GAME_STATE_STARTUP:
			return "startup";
		case GAME_STATE_INIT:
			return "init";
		case GAME_STATE_NORMAL:
			return "normal";
		case GAME_STATE_CLOSED:
			return "closed";
		case GAME_STATE_SHUTDOWN:
			return "shutdown";
		case GAME_STATE_CLOSING:
			return "closing";
		case GAME_STATE_MAINTAIN:
			return "maintain";
		default:
			return "unknown";
	}
}

std::string buildHttpResponse(int statusCode, const std::string &statusText, const std::string &jsonBody) {
	return fmt::format(
		"HTTP/1.1 {} {}\r\n"
		"Content-Type: application/json; charset=utf-8\r\n"
		"Content-Length: {}\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store\r\n"
		"\r\n"
		"{}",
		statusCode,
		statusText,
		jsonBody.size(),
		jsonBody
	);
}

std::string trimCommandPath(const std::string &path) {
	static constexpr std::string_view prefix = "/command/";
	if (path.rfind(prefix, 0) == 0) {
		return path.substr(prefix.size());
	}
	return {};
}

} // namespace

PythonAdminBridge::PythonAdminBridge(Logger &logger) :
	logger(logger) {
	if (const char* disabled = std::getenv("CANARY_PYTHON_BRIDGE_DISABLED")) {
		enabled = std::string_view(disabled) != "1";
	}

	if (const char* customPort = std::getenv("CANARY_PYTHON_BRIDGE_PORT")) {
		const auto parsed = std::atoi(customPort);
		if (parsed > 0 && parsed <= 65535) {
			port = static_cast<uint16_t>(parsed);
		}
	}
}

PythonAdminBridge::~PythonAdminBridge() {
	stop();
}

void PythonAdminBridge::start() {
	if (!enabled || running.exchange(true)) {
		return;
	}

	try {
		ioContext = std::make_unique<asio::io_context>();
		acceptor = std::make_unique<asio::ip::tcp::acceptor>(
			*ioContext,
			asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)
		);
		acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
		worker = std::thread(&PythonAdminBridge::runLoop, this);
		logger.info("Python admin bridge listening on http://127.0.0.1:{}", port);
	} catch (const std::exception &ex) {
		running = false;
		ioContext.reset();
		acceptor.reset();
		logger.error("Python admin bridge could not start: {}", ex.what());
	}
}

void PythonAdminBridge::stop() {
	if (!running.exchange(false)) {
		return;
	}

	if (acceptor) {
		asio::error_code ec;
		acceptor->close(ec);
	}

	if (ioContext) {
		ioContext->stop();
	}

	if (worker.joinable()) {
		worker.join();
	}

	acceptor.reset();
	ioContext.reset();
}

void PythonAdminBridge::runLoop() {
	while (running) {
		asio::ip::tcp::socket socket(*ioContext);
		asio::error_code ec;
		acceptor->accept(socket, ec);
		if (ec) {
			if (running) {
				logger.warn("Python admin bridge accept failed: {}", ec.message());
			}
			continue;
		}

		try {
			asio::streambuf requestBuffer;
			asio::read_until(socket, requestBuffer, "\r\n\r\n", ec);
			if (ec && ec != asio::error::eof) {
				logger.warn("Python admin bridge read failed: {}", ec.message());
				continue;
			}

			std::istream requestStream(&requestBuffer);
			std::string requestText((std::istreambuf_iterator<char>(requestStream)), std::istreambuf_iterator<char>());
			const auto response = handleRequest(requestText);
			asio::write(socket, asio::buffer(response), ec);
		} catch (const std::exception &ex) {
			logger.error("Python admin bridge request failed: {}", ex.what());
		}
	}
}

std::string PythonAdminBridge::handleRequest(const std::string &requestText) const {
	std::istringstream stream(requestText);
	std::string method;
	std::string path;
	std::string version;
	stream >> method >> path >> version;

	if (method == "GET" && path == "/status") {
		return buildHttpResponse(200, "OK", buildStatusJson());
	}

	if (method == "POST") {
		const auto command = trimCommandPath(path);
		if (!command.empty()) {
			return buildHttpResponse(200, "OK", executeCommand(command));
		}
	}

	return buildHttpResponse(404, "Not Found", R"({"ok":false,"error":"not_found"})");
}

std::string PythonAdminBridge::buildStatusJson() const {
	return fmt::format(
		R"({{"ok":true,"bridge":"python_admin","port":{},"players_online":{},"game_state":"{}"}})",
		port,
		g_game().getPlayersOnline(),
		escapeJson(gameStateToString(g_game().getGameState()))
	);
}

std::string PythonAdminBridge::executeCommand(const std::string &command) const {
	auto promise = std::make_shared<std::promise<std::string>>();
	auto future = promise->get_future();

	g_dispatcher().addEvent(
		[command, promise]() mutable {
			try {
				if (command == "save") {
					g_saveManager().saveAll();
					promise->set_value(R"({"ok":true,"command":"save","result":"save_all"})");
					return;
				}

				if (command == "reload-all") {
					const bool result = GameReload::init(Reload_t::RELOAD_TYPE_ALL);
					promise->set_value(fmt::format(R"({{"ok":{},"command":"reload-all"}})", result ? "true" : "false"));
					return;
				}

				if (command == "open") {
					g_game().setGameState(GAME_STATE_NORMAL);
					promise->set_value(R"({"ok":true,"command":"open","game_state":"normal"})");
					return;
				}

				if (command == "close") {
					g_game().setGameState(GAME_STATE_CLOSED);
					promise->set_value(R"({"ok":true,"command":"close","game_state":"closed"})");
					return;
				}

				if (command == "shutdown") {
					g_game().setGameState(GAME_STATE_SHUTDOWN);
					promise->set_value(R"({"ok":true,"command":"shutdown","game_state":"shutdown"})");
					return;
				}

				promise->set_value(fmt::format(R"({{"ok":false,"error":"unknown_command","command":"{}"}})", escapeJson(command)));
			} catch (const std::exception &ex) {
				promise->set_value(fmt::format(R"({{"ok":false,"error":"{}","command":"{}"}})", escapeJson(ex.what()), escapeJson(command)));
			}
		},
		__FUNCTION__
	);

	if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
		return fmt::format(R"({{"ok":false,"error":"timeout","command":"{}"}})", escapeJson(command));
	}

	return future.get();
}
