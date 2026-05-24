/**
 * @file chunk_upload_cgi.cpp
 * @brief 接收单个分片数据并保存到临时目录，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include "cfg.h"
#include "make_log.h"
#include "redis_op.h"
#include "util_cgi.h"
}

#define CHUNK_LOG_MODULE  "cgi"
#define CHUNK_LOG_PROC    "chunk_upload"
#define CHUNK_TEMP_DIR    "/tmp/chunks"

namespace {

char redis_ip[30] = {0};
char redis_port[10] = {0};

using RedisPtr = std::unique_ptr<redisContext, decltype(&rop_disconnect)>;

struct FdCloser {
    void operator()(int *fd) const
    {
        if (fd != nullptr) {
            if (*fd >= 0) {
                close(*fd);
            }
            delete fd;
        }
    }
};

using FdPtr = std::unique_ptr<int, FdCloser>;

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

bool read_exact(std::vector<char> &buffer, long len)
{
    long total_read = 0;
    while (total_read < len) {
        int n = FCGI_fread(buffer.data() + total_read, 1, len - total_read, stdin);
        if (n <= 0) {
            break;
        }
        total_read += n;
    }

    if (total_read != len) {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "fread incomplete: expected=%ld, got=%ld\n", len, total_read);
        return false;
    }
    return true;
}

bool write_exact(int fd, const std::vector<char> &buffer, long len)
{
    long total_written = 0;
    while (total_written < len) {
        int n = write(fd, buffer.data() + total_written, len - total_written);
        if (n <= 0) {
            break;
        }
        total_written += n;
    }

    if (total_written != len) {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "write incomplete\n");
        return false;
    }
    return true;
}

void update_uploaded_chunks(const char *file_md5, int chunk_index)
{
    RedisPtr redis_conn(rop_connectdb_nopwd(redis_ip, redis_port), rop_disconnect);
    if (redis_conn == nullptr) {
        return;
    }

    char redis_key[512] = {0};
    snprintf(redis_key, sizeof(redis_key), "chunk:%s", file_md5);

    char uploaded[1024] = {0};
    int r = rop_hash_get(redis_conn.get(), redis_key, const_cast<char *>("uploaded"), uploaded);

    char new_uploaded[1024] = {0};
    if (r == 0 && strlen(uploaded) > 0) {
        snprintf(new_uploaded, sizeof(new_uploaded), "%s,%d", uploaded, chunk_index);
    } else {
        snprintf(new_uploaded, sizeof(new_uploaded), "%d", chunk_index);
    }

    rop_hash_set(redis_conn.get(), redis_key, const_cast<char *>("uploaded"), new_uploaded);
}

} // namespace

int main()
{
    read_cfg();

    while (FCGI_Accept() >= 0) {
        char *content_length = getenv("CONTENT_LENGTH");
        char *query_string = getenv("QUERY_STRING");
        long len = content_length == nullptr ? 0 : strtol(content_length, nullptr, 10);

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0 || query_string == nullptr) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "no data or no query string\n");
            print_json_code(1);
            continue;
        }

        char file_md5[256] = {0};
        char index_str[32] = {0};
        int value_len = 0;

        if (query_parse_key_value(query_string, "md5", file_md5, &value_len) != 0) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "parse md5 from query err\n");
            print_json_code(1);
            continue;
        }

        if (query_parse_key_value(query_string, "index", index_str, &value_len) != 0) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "parse index from query err\n");
            print_json_code(1);
            continue;
        }

        int chunk_index = atoi(index_str);

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "receiving chunk: md5=%s, index=%d, size=%ld\n",
                file_md5, chunk_index, len);

        std::vector<char> chunk_buf(static_cast<size_t>(len));
        if (!read_exact(chunk_buf, len)) {
            print_json_code(1);
            continue;
        }

        char chunk_path[512] = {0};
        snprintf(chunk_path, sizeof(chunk_path), "%s/%s/%d",
                CHUNK_TEMP_DIR, file_md5, chunk_index);

        FdPtr fd(new int(open(chunk_path, O_CREAT | O_WRONLY | O_TRUNC, 0644)));
        if (*fd < 0) {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                    "open %s err: %s\n", chunk_path, strerror(errno));
            print_json_code(1);
            continue;
        }

        if (!write_exact(*fd, chunk_buf, len)) {
            print_json_code(1);
            continue;
        }

        update_uploaded_chunks(file_md5, chunk_index);

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "chunk saved: %s (%ld bytes)\n", chunk_path, len);

        print_json_code(0);
    }

    return 0;
}
