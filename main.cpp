#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <regex>
#include <fstream>
#include <iostream>


using requests_t = std::deque<std::string>;
using correlated_resources_t = std::map<const std::string, const std::string>;
using response_map_t = std::map<const std::string, std::string>;

const std::string DEFAULT_PORT = "8080";
const unsigned BUFFER_SIZE = 1024;

void create_urls_for_correlated_resources(
        const std::string &filename,
        correlated_resources_t &correlated_resources
) {
    std::string prot = "http://";
    std::string colon = ":";

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Unable to open a file: " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    for (std::string line; std::getline(file, line);) {
        char delim = '\t';
        std::istringstream iss(line);
        std::string resrc, server, port;

        std::getline(iss, resrc, delim);
        std::getline(iss, server, delim);
        std::getline(iss, port, delim);

        std::string url = prot + server + colon + port + resrc;
        correlated_resources.insert({resrc, url});
    }

    file.close();
}

void process_start_line_handle_target_file(
        const std::string &target_path,
        const std::string &directory,
        FILE *file,
        response_map_t &response_map
) {
    char *target_act_path = realpath(target_path.c_str(), nullptr);
    char *directory_act_path = realpath(directory.c_str(), nullptr);
    if (target_act_path != nullptr && directory_act_path != nullptr) {
        // Check if directory_act_path is a prefix of target_act_path.
        if (strlen(directory_act_path) < strlen(target_act_path) &&
            std::string(target_act_path).rfind(directory_act_path, 0) == 0) {
            fseek(file, 0, SEEK_END);
            long file_length = ftell(file);
            fseek(file, 0, SEEK_SET);
            char *buffer = (char *) malloc((file_length + 1) * sizeof(char));
            if (buffer != nullptr) {
                fread(buffer, sizeof(char), file_length, file);
                buffer[file_length] = '\0';
                std::string message_body = buffer;

                response_map.insert({"status-code", "200"});
                response_map.insert({"reason-phrase", "OK"});
                response_map.insert({"message-body", message_body});
                response_map.insert(
                        {"content-length", std::to_string(message_body.length())}
                );
                response_map.insert({"content-type", "application/octet-stream"});

                free(buffer);
            }
            else {
                response_map.insert({"status-code", "500"});
                response_map.insert({"reason-phrase", "Internal Server Error"});
            }
        }
        else {
            response_map.insert({"status-code", "404"});
            response_map.insert({"reason-phrase", "Not Found"});
        }
    }
    else {
        response_map.insert({"status-code", "500"});
        response_map.insert({"reason-phrase", "Internal Server Error"});
    }

    free(directory_act_path);
    free(target_act_path);
    fclose(file);
}

// Return first pos in request after start line.
size_t process_start_line(
        const std::string &request,
        const std::string &start_line_pattern,
        const std::string &directory,
        const correlated_resources_t &correlated_resources,
        response_map_t &response_map
) {
    std::smatch match;
    std::regex start_line_regex(start_line_pattern);
    std::regex_search(request, match, start_line_regex);

    std::string method, target, version;
    std::stringstream start_line_stream(match[0]);
    start_line_stream >> method >> target >> version;

    if (method != "GET" && method != "HEAD") {
        response_map.insert({"status-code", "501"});
        response_map.insert({"reason-phrase", "Not Implemented"});
    }
    else {
        std::string target_path = directory == "/" ? target : directory + target;
        struct stat sb;
        FILE *file = fopen(target_path.c_str(), "r");
        if (stat(target_path.c_str(), &sb) >= 0 &&
            S_ISREG(sb.st_mode) != 0 && file != nullptr) {
            process_start_line_handle_target_file(
                    target_path,
                    directory,
                    file,
                    response_map
            );
        }
        else {
            auto resrc_it = correlated_resources.find(target);
            if (resrc_it == correlated_resources.end()) {
                response_map.insert({"status-code", "404"});
                response_map.insert({"reason-phrase", "Not Found"});
            }
            else {
                response_map.insert({"status-code", "302"});
                response_map.insert({"reason-phrase", "Found"});
                response_map.insert({"location", resrc_it->second});
            }
        }
    }

    if (method == "HEAD") {
        response_map.erase("message-body");
    }

    response_map.insert({"content-length", "0"});

    return match[0].length();
}

void process_header_field_lines(
        const std::string &request,
        const std::string &header_field_pattern,
        response_map_t &response_map
) {
    std::regex header_field_regex(header_field_pattern);
    bool bad_format = false;
    bool content_length_occurred = false;

    std::sregex_iterator regex_it = std::sregex_iterator(
            request.begin(),
            request.end(),
            header_field_regex
    );
    for (; regex_it != std::sregex_iterator(); regex_it++) {
        std::smatch match = *regex_it;
        std::string field_name, field_value;
        std::stringstream line_stream(match.str());
        line_stream >> field_name >> field_value;

        for (size_t i = 0; i < field_name.length(); i++) {
            field_name[i] = std::tolower(field_name[i]);
        }
        if (field_value != "") {
            field_name.pop_back(); // Remove colon.
        }
        else {
            size_t colon_pos = field_name.find(':');
            field_value = field_name.substr(colon_pos + 1);
            field_name = field_name.substr(0, colon_pos);
        }

        if (field_name == "connection") {
            if (response_map.find(field_name) != response_map.end()) {
                bad_format = true;
            }
            else {
                response_map.insert({field_name, field_value});
            }
        }
        else if (field_name == "content-length") {
            if (content_length_occurred || field_value != "0") {
                bad_format = true;
            }
            else {
                content_length_occurred = true;
            }
        }
    }

    if (response_map["status-code"] == "501") {
        response_map.insert({"connection", "close"});
    }
    else {
        response_map.insert({"connection", "keep-alive"});
    }

    if (bad_format) {
        response_map["status-code"] = "400";
        response_map["reason-phrase"] = "Bad Request";
        response_map["connection"] = "close";
        response_map.erase("message-body");
        response_map["content-length"] = "0";
        response_map.erase("content-type");
    }
}

response_map_t prepare_response_map(
        const std::string &request,
        const std::string &directory,
        const correlated_resources_t &correlated_resources
) {
    std::cout << "Request:\n" << request << std::endl << std::endl;

    std::smatch match;
    std::string start_line_pattern(
            R"(^[^ ]+ /[^ ]* HTTP/1.1\r\n)"
    );
    std::string header_field_pattern(R"([^ ]+: *[^ ]+ *\r\n)");
    std::string ending_line_pattern(R"(\r\n$)");
    std::regex request_regex(
            start_line_pattern + "(" + header_field_pattern + ")*" + ending_line_pattern
    );

    response_map_t response_map;
    response_map.insert({"http-version", "HTTP/1.1"});

    if (!std::regex_match(request, match, request_regex)) {
        response_map.insert({"status-code", "400"});
        response_map.insert({"connection", "close"});
        response_map.insert({"content-length", "0"});
    }
    else {
        size_t start = process_start_line(
                request,
                start_line_pattern,
                directory,
                correlated_resources,
                response_map
        );
        process_header_field_lines(
                request.substr(start),
                header_field_pattern,
                response_map
        );
    }

    return response_map;
}

const std::string prepare_response(response_map_t &response_map) {
    std::string status_line =
            response_map["http-version"] + " " + response_map["status-code"] + " " +
            response_map["reason-phrase"] + "\r\n";
    std::string connection_line =
            "Connection: " + response_map["connection"] + "\r\n";
    std::string content_type_line;
    std::string content_length_line =
            "Content-length: " + response_map["content-length"] + "\r\n";
    std::string location_line;
    std::string message_body = response_map["message-body"];

    if (response_map.find("content-type") != response_map.end()) {
        content_type_line = "Content-type: " + response_map["content-type"] + "\r\n";
    }
    if (response_map.find("location") != response_map.end()) {
        content_type_line = "Location: " + response_map["location"] + "\r\n";
    }

    std::string response;
    response += status_line;
    response += connection_line;
    response += content_type_line;
    response += content_length_line;
    response += location_line;
    response += "\r\n";
    response += message_body;

    if (response.length() < 200) {
        std::cout << "Response:\n" << response << std::endl << std::endl;
    }

    return response;
}

void send_response(int client_sock, const std::string &response) {
    const char *buffer = response.c_str();
    ssize_t written;
    size_t start = 0;
    size_t to_write = strlen(buffer);

    do {
        written = write(client_sock, buffer + start, to_write);
        if (written < 0) {
            //TODO: writing failed
            exit(EXIT_FAILURE);
        }

        start += written;
        to_write -= written;
    } while (to_write > 0);
}

void handle_client(
        int client_sock,
        const std::string &directory,
        const correlated_resources_t &correlated_resources
) {
    size_t start = 0;
    char buffer[BUFFER_SIZE];
    // Number of bytes read from the buffer.
    ssize_t bytes_read;
    // If true, then new buffer read deals with a new request, false otherwise.
    bool new_request = true;
    std::string request;
    // Last four characters read to determine when current request ends.
    std::string last_four_chars;

    do {
        if (start == 0) {
            bytes_read = read(client_sock, buffer, sizeof(char) * BUFFER_SIZE);
            if (bytes_read < 0) {
                //TODO: reading failure
                exit(EXIT_FAILURE);
            }
            else if (bytes_read == 0) {
                // Client disconnected.
                break;
            }
        }

        if (new_request) {
            new_request = false;
        }

        size_t i;
        for (i = start; i < bytes_read && !new_request; i++) {
            request += buffer[i];
            if (last_four_chars.length() == 4) {
                last_four_chars = last_four_chars.substr(1);
            }
            last_four_chars += buffer[i];

            // Check if the current request ended.
            if (last_four_chars == "\r\n\r\n") {
                new_request = true;
            }
        }

        if (new_request) {
            // Handle current request first before dealing with the next request.
            response_map_t response_map = prepare_response_map(
                    request,
                    directory,
                    correlated_resources
            );
            const std::string response = prepare_response(response_map);
            send_response(client_sock, response);

            if (response_map["connection"] == "close") {
                // Close connection with the client.
                break;
            }

            // Prepare for a new request.
            request.clear();
            last_four_chars.clear();
        }

        start = i % bytes_read;
    } while (bytes_read > 0);

    printf("ending connection\n");

    if (close(client_sock) < 0) {
        //TODO: closing connection failure
        exit(EXIT_FAILURE);
    }
}

void validate_command_line_arguments(
        const std::string &directory,
        const std::string &filename,
        const std::string &port_str
) {
    struct stat sb;
    if (stat(directory.c_str(), &sb) < 0 || S_ISDIR(sb.st_mode) == 0) {
        std::cerr << "Not a regular directory: " << std::strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }
    if (stat(filename.c_str(), &sb) < 0 || S_ISREG(sb.st_mode) == 0) {
        std::cerr << "Not a regular file: " << std::strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    std::smatch match;
    // https://stackoverflow.com/questions/12968093/regex-to-validate-port-number
    std::regex port_regex(
            R"(^[0-9]{1,4}|[1-5][0-9]{4}|6[0-4][0-9]{3}|)"
            R"(65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5]$)"
    );
    if (!std::regex_match(port_str, match, port_regex)) {
        std::cerr << "Not a valid port: " << std::endl;
        exit(EXIT_FAILURE);
    }
}

void handle_clients(
        int sock,
        const std::string &directory,
        const correlated_resources_t &correlated_resources
) {
    for (;;) {
        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);
        int client_sock = accept(
                sock, (struct sockaddr *) &client_address, &client_address_len
        );
        if (client_sock < 0) {
            std::cerr << "Accepting client failed: " << std::strerror(errno)
                      << std::endl;
            exit(EXIT_FAILURE);
        }

        handle_client(client_sock, directory, correlated_resources);
    }
}

int create_and_prepare_socket_for_accepting_clients(unsigned port_num) {
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed: " << std::strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_num);
    if (bind(sock, (struct sockaddr *) &address, sizeof(address)) < 0) {
        std::cerr << "Socket binding failed: " << std::strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    if (listen(sock, SOMAXCONN) < 0) {
        std::cerr << "Socket listening failed: " << std::strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    return sock;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        //TODO: invalid program invocation
        exit(EXIT_FAILURE);
    }

    std::string directory = argv[1];
    if (directory[directory.length() - 1] == '/' && directory.length() > 1) {
        directory.pop_back();
    }
    std::string filename = argv[2];
    std::string port_str = argc == 4 ? std::string(argv[3]) : DEFAULT_PORT;
    validate_command_line_arguments(directory, filename, port_str);
    const unsigned port_num = stoi(port_str);

    correlated_resources_t correlated_resources;
    create_urls_for_correlated_resources(filename, correlated_resources);

    int sock = create_and_prepare_socket_for_accepting_clients(port_num);
    handle_clients(sock, directory, correlated_resources);

    return 0;
}
