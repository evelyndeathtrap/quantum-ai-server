#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <curl/curl.h>
#include <cjson/cJSON.h> // Include cJSON for parsing

#define HTTP_PORT 443
#define UDP_PORT 443
#define BUFFER_SIZE 65536

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if(!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// NEW: Helper function to parse Gemini's JSON structure and extract ONLY the text response
char* extract_text_from_json(const char *raw_json) {
    cJSON *json = cJSON_Parse(raw_json);
    if (!json) {
        return strdup("Error: Failed to parse JSON response.");
    }

    cJSON *candidates = cJSON_GetObjectItemCaseSensitive(json, "candidates");
    if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        cJSON *first_candidate = cJSON_GetArrayItem(candidates, 0);
        cJSON *content = cJSON_GetObjectItemCaseSensitive(first_candidate, "content");
        cJSON *parts = cJSON_GetObjectItemCaseSensitive(content, "parts");
        
        if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
            cJSON *first_part = cJSON_GetArrayItem(parts, 0);
            cJSON *text = cJSON_GetObjectItemCaseSensitive(first_part, "text");
            
            if (cJSON_IsString(text) && (text->valuestring != NULL)) {
                char *extracted = strdup(text->valuestring);
                cJSON_Delete(json);
                return extracted;
            }
        }
    }

    cJSON_Delete(json);
    return strdup("Error: Could not locate text field in Gemini API response.");
}

char* read_api_key() {
    FILE *f = fopen("GEMINI_API_KEY", "r");
    if (!f) {
        perror("Failed to open GEMINI_API_KEY file");
        exit(EXIT_FAILURE);
    }
    char *key = malloc(256);
    if (fscanf(f, "%255s", key) != 1) {
        fprintf(stderr, "Error reading API key from file.\n");
        exit(EXIT_FAILURE);
    }
    fclose(f);
    return key;
}

char* call_gemini_api(const char *api_key, const char *prompt, int is_html_mode) {
    CURL *curl_handle;
    CURLcode res;
    struct MemoryStruct chunk;
    chunk.memory = malloc(1); 
    chunk.size = 0; 

    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle = curl_easy_init();

    char url[512];
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=%s", api_key);

    const char *system_prompt = "you are very terse, belong to a secretive elitist club and really female";
    const char *format_instruction = is_html_mode ? 
        " Return your response inside a clean, styled HTML webpage structure. Include standard <html>, <head> with inline CSS, and <body> tags. Do not wrap the response in markdown blocks like ```html." : 
        " Respond concisely in plain text as instructed.";

    char payload[BUFFER_SIZE];
    snprintf(payload, sizeof(payload),
        "{"
        "  \"systemInstruction\": {\"parts\": [{\"text\": \"%s\"}]},"
        "  \"contents\": [{\"parts\": [{\"text\": \"%s %s\"}]}]"
        "}", system_prompt, prompt, format_instruction);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl_handle);

    char *final_text = NULL;
    if(res != CURLE_OK) {
        size_t err_msg_len = strlen(curl_easy_strerror(res)) + 32;
        final_text = malloc(err_msg_len);
        snprintf(final_text, err_msg_len, "CURL Error: %s", curl_easy_strerror(res));
    } else {
        // Parse the raw JSON payload and extract ONLY the text response
        final_text = extract_text_from_json(chunk.memory);
    }

    free(chunk.memory);
    curl_easy_cleanup(curl_handle);
    curl_slist_free_all(headers);
    curl_global_cleanup();

    return final_text; 
}

int main() {
    char *api_key = read_api_key();
    printf("API Key loaded successfully.\n");

    int http_fd, udp_fd;
    struct sockaddr_in http_addr, udp_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // --- Setup HTTP Socket ---
    if ((http_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) { perror("HTTP Socket failed"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = htons(HTTP_PORT);
    if (bind(http_fd, (struct sockaddr *)&http_addr, sizeof(http_addr)) < 0) { perror("HTTP Bind failed"); exit(EXIT_FAILURE); }
    if (listen(http_fd, 10) < 0) { perror("HTTP Listen failed"); exit(EXIT_FAILURE); }

    // --- Setup UDP Socket ---
    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { perror("UDP Socket failed"); exit(EXIT_FAILURE); }
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(UDP_PORT);
    if (bind(udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) { perror("UDP Bind failed"); exit(EXIT_FAILURE); }

    printf("HTTP Web Server running on port %d\n", HTTP_PORT);
    printf("UDP Chat Server running on port %d\n\n", UDP_PORT);

    fd_set readfds;
    int max_fd = (http_fd > udp_fd) ? http_fd : udp_fd;

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(http_fd, &readfds);
        FD_SET(udp_fd, &readfds);

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) { perror("Select error"); continue; }

        // --- Handle HTTP Request ---
        if (FD_ISSET(http_fd, &readfds)) {
            int new_socket = accept(http_fd, (struct sockaddr *)&client_addr, &addr_len);
            memset(buffer, 0, BUFFER_SIZE);
            read(new_socket, buffer, BUFFER_SIZE);

            char prompt[512] = "Hello";
            char *get_ptr = strstr(buffer, "GET /");
            if (get_ptr) {
                char *space_ptr = strchr(get_ptr + 5, ' ');
                if (space_ptr) {
                    size_t len = space_ptr - (get_ptr + 5);
                    if (len > 0 && len < 511) {
                        strncpy(prompt, get_ptr + 5, len);
                        prompt[len] = '\0';
                    }
                }
            }

            printf("[HTTP] Prompt received: %s\n", prompt);
            char *gemini_res = call_gemini_api(api_key, prompt, 1); 

            char http_header[BUFFER_SIZE];
            snprintf(http_header, sizeof(http_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: close\r\n\r\n"
                     "%s", strlen(gemini_res), gemini_res);

            write(new_socket, http_header, strlen(http_header));
            free(gemini_res);
            close(new_socket);
        }

        // --- Handle UDP Chat Message ---
        if (FD_ISSET(udp_fd, &readfds)) {
            memset(buffer, 0, BUFFER_SIZE);
            int len = recvfrom(udp_fd, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
            buffer[len] = '\0';

            if(buffer[len-1] == '\n') buffer[len-1] = '\0';

            printf("[UDP] Prompt received: %s\n", buffer);
            char *gemini_res = call_gemini_api(api_key, buffer, 0); 

            sendto(udp_fd, gemini_res, strlen(gemini_res), 0, (struct sockaddr *)&client_addr, addr_len);
            free(gemini_res);
        }
    }

    free(api_key);
    return 0;
}
