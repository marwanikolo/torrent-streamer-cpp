#include "CliParser.h"
#include "Utils.h" // Add this include
#include <cstdlib>
#include <string>
#include <print>

AppConfig parse_cli_args(int argc, char* argv[]) {
    AppConfig config;
    config.save_dir = "/mnt/NewVolume/Tordown";
    if (config.port <= 0) config.port = 8080; 
    
    // Securely check for Environment Variables
    if (const char* env_token = std::getenv("GOFILE_TOKEN")) {
        config.gofile_token = env_token;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) config.port = std::stoi(argv[++i]);
        else if (arg == "-d" && i + 1 < argc) config.save_dir = argv[++i];
        else if (arg == "--player" && i + 1 < argc) config.player_path = argv[++i];
        else if (arg == "--debug" || arg == "-v") config.debug_mode = true;
        else if (arg == "--gofile-token" && i + 1 < argc) config.gofile_token = argv[++i];
        else if (arg == "--user-agent" && i + 1 < argc) config.custom_user_agent = argv[++i];
        else if (arg == "--referer" && i + 1 < argc) config.custom_referer = argv[++i];
        
        // NEW: Intercept Burp File and dump it into custom_headers
        else if ((arg == "-b" || arg == "--burp") && i + 1 < argc) {
            std::string burp_path = argv[++i];
            auto extracted_headers = parse_burp_file(burp_path);
            
            // Insert all found headers into the existing custom_headers config
            config.custom_headers.insert(
                config.custom_headers.end(), 
                extracted_headers.begin(), 
                extracted_headers.end()
            );
        }
        
        // Arbitrary Header Parser (-H or --header)
        else if ((arg == "-H" || arg == "--header") && i + 1 < argc) {
            std::string header_str = argv[++i];
            size_t colon_pos = header_str.find(':');
            
            if (colon_pos != std::string::npos) {
                std::string key = header_str.substr(0, colon_pos);
                std::string value = header_str.substr(colon_pos + 1);
                
                size_t start = value.find_first_not_of(" \t");
                if (start != std::string::npos) value = value.substr(start);
                else value = "";
                
                config.custom_headers.push_back({key, value});
            } else {
                std::println(stderr, "[-] Warning: Invalid header format '{}'. Expected 'Key: Value'", header_str);
            }
        }
        else {
            config.initial_source = arg;
        }
    }
    return config;
}