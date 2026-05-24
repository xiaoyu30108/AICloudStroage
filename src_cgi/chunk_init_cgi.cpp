/**
 * @file chunk_init_cgi.cpp
 * @brief 分片上传初始化 CGI，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" {
#include "cfg.h"
#include "cJSON.h"
#include "make_log.h"
#include "redis_op.h"
#include "util_cgi.h"
}

#define CHUNK_LOG_MODULE  "cgi"
#define CHUNK_LOG_PROC    "chunk_init"
#define CHUNK_TEMP_DIR    "/tmp/chunks"

namespace {

char redis_ip[30] = {0};
char redis_port[10] = {0};

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using RedisPtr = std::unique_ptr<redisContext, decltype(&rop_disconnect)>;

void read_cfg()
{
    get_cfg_value(CFG_PATH, "redis", "ip", redis_ip);
    get_cfg_value(CFG_PATH, "redis", "port", redis_port);
    LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "redis:[ip=%s,port=%s]", redis_ip, redis_port);
}

void print_json_code(int code)
{
    FCGI_printf("{\"code\":%d}", code);
}

bool copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == nullptr || item->valuestring == nullptr) {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "cJSON_GetObjectItem %s err\n", key);
        return false;
    }

    snprintf(dst, dst_size, "%s", item->valuestring);
    return true;
}

int parse_init_json(const char *buf, char *user, size_t user_size,
        char *token, size_t token_size, char *filename, size_t filename_size,
        char *file_md5, size_t md5_size, long *filesize, int *chunk_count)
{
    JsonPtr root(cJSON_Parse(buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    if (!copy_json_string(root.get(), "user", user, user_size) ||
            !copy_json_string(root.get(), "token", token, token_size) ||
            !copy_json_string(root.get(), "filename", filename, filename_size) ||
            !copy_json_string(root.get(), "md5", file_md5, md5_size)) {
        return -1;
    }

    cJSON *size_item = cJSON_GetObjectItem(root.get(), "size");
    cJSON *count_item = cJSON_GetObjectItem(root.get(), "chunkCount");
    if (size_item == nullptr || count_item == nullptr) {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "size/chunkCount json err\n");
        return -1;
    }

    *filesize = static_cast<long>(size_item->valuedouble);
    *chunk_count = count_item->valueint;
    return 0;
}

void print_init_response(int chunk_count, const char *uploaded_chunks)
{
    JsonPtr resp(cJSON_CreateObject(), cJSON_Delete);
    if (resp == nullptr) {
        print_json_code(1);
        return;
    }

    cJSON_AddNumberToObject(resp.get(), "code", 0);
    cJSON_AddNumberToObject(resp.get(), "chunkCount", chunk_count);
    cJSON_AddStringToObject(resp.get(), "uploadedChunks", uploaded_chunks);

    char *out = cJSON_PrintUnformatted(resp.get());
    if (out != nullptr) {
        FCGI_printf("%s", out);
        free(out);
    }
}

} // namespace

int main()
{
    read_cfg();
    mkdir(CHUNK_TEMP_DIR, 0755);

    while (FCGI_Accept() >= 0) {
        char *content_length = getenv("CONTENT_LENGTH");
        int len = content_length == nullptr ? 0 : atoi(content_length);

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0) {
            print_json_code(1);
            continue;
        }

        char buf[4 * 1024] = {0};
        int ret = FCGI_fread(buf, 1, len, stdin);
        if (ret == 0) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "fread stdin err\n");
            print_json_code(1);
            continue;
        }

        char user[128] = {0};
        char token[256] = {0};
        char filename[256] = {0};
        char file_md5[256] = {0};
        long filesize = 0;
        int chunk_count = 0;

        if (parse_init_json(buf, user, sizeof(user), token, sizeof(token),
                filename, sizeof(filename), file_md5, sizeof(file_md5),
                &filesize, &chunk_count) != 0) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "parse_init_json err\n");
            print_json_code(1);
            continue;
        }

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "user=%s, md5=%s, filename=%s, size=%ld, chunks=%d\n",
                user, file_md5, filename, filesize, chunk_count);

        ret = verify_token(user, token);
        if (ret != 0) {
            print_json_code(4);
            continue;
        }

        RedisPtr redis_conn(rop_connectdb_nopwd(redis_ip, redis_port), rop_disconnect);
        if (redis_conn == nullptr) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "redis connect err\n");
            print_json_code(1);
            continue;
        }

        char redis_key[512] = {0};
        snprintf(redis_key, sizeof(redis_key), "chunk:%s", file_md5);

        int key_exist = rop_is_key_exist(redis_conn.get(), redis_key);
        if (key_exist == 1) {
            char uploaded[1024] = {0};
            int r = rop_hash_get(redis_conn.get(), redis_key, const_cast<char *>("uploaded"), uploaded);
            if (r != 0) {
                uploaded[0] = '\0';
            }

            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                    "chunk key exists, uploaded=%s\n", uploaded);
            print_init_response(chunk_count, uploaded);
            continue;
        }

        rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("filename"), filename);

        char size_str[64] = {0};
        snprintf(size_str, sizeof(size_str), "%ld", filesize);
        rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("filesize"), size_str);

        char count_str[32] = {0};
        snprintf(count_str, sizeof(count_str), "%d", chunk_count);
        rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("chunk_count"), count_str);

        rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("user"), user);
        rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("uploaded"), const_cast<char *>(""));

        redisReply *reply = static_cast<redisReply *>(
                redisCommand(redis_conn.get(), "EXPIRE %s 86400", redis_key));
        if (reply != nullptr) {
            freeReplyObject(reply);
        }

        char chunk_dir[512] = {0};
        snprintf(chunk_dir, sizeof(chunk_dir), "%s/%s", CHUNK_TEMP_DIR, file_md5);
        mkdir(chunk_dir, 0755);

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "chunk init OK, dir=%s, chunks=%d\n", chunk_dir, chunk_count);

        print_init_response(chunk_count, "");
    }

    return 0;
}
