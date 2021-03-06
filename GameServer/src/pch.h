#pragma once
// Standard libraries
#include <vector>
#include <regex>
#include <thread>
#include <iostream>
#include <ctime>
#include <string>
#include <mutex>
#include <random>

#ifdef __linux__
	#include <winsock2.h>
	#include <experimental/filesystem>
#elif _WIN32
	#include <filesystem>

// Guidelines Support Library
#undef max
#include <gsl/gsl>

// Logging library
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// SCL - configuration file library
#include "SCL/SCL.hpp"

typedef std::mt19937 default_random_engine;

#pragma comment(lib,"WS2_32")
