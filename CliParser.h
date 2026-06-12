#pragma once
#include "Config.h"

// Parses command line arguments and environment variables into a clean struct
AppConfig parse_cli_args(int argc, char* argv[]);
