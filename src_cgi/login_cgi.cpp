/**
 * @file login_cgi.cpp
 * @brief 登录后台 CGI 程序，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

extern "C" {
#include "base64.h"
#include "cfg.h"
#include "cJSON.h"
#include "deal_mysql.h"
#include "des.h"
#include "make_log.h"
#include "md5.h"
#include "redis_op.h"
#include "util_cgi.h"
}

#define LOGIN_LOG_MODULE "cgi"
#define LOGIN_LOG_PROC   "login"

namespace {

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using RedisPtr = std::unique_ptr<redisContext, decltype(&rop_disconnect)>;

struct MysqlDeleter {
    void operator()(MYSQL *conn) const
    {
        if (conn != nullptr) {
            mysql_close(conn);
        }
    }
};

using MysqlPtr = std::unique_ptr<MYSQL, MysqlDeleter>;

int get_login_info(const char *login_buf, char *username, size_t username_size,
        char *pwd, size_t pwd_size)
{
    JsonPtr root(cJSON_Parse(login_buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    cJSON *user_item = cJSON_GetObjectItem(root.get(), "user");
    cJSON *pwd_item = cJSON_GetObjectItem(root.get(), "pwd");
    if (user_item == nullptr || pwd_item == nullptr ||
            user_item->valuestring == nullptr || pwd_item->valuestring == nullptr) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "invalid login json\n");
        return -1;
    }

    snprintf(username, username_size, "%s", user_item->valuestring);
    snprintf(pwd, pwd_size, "%s", pwd_item->valuestring);
    return 0;
}

std::string compute_md5_hex(const unsigned char *input, unsigned int input_len)
{
    MD5_CTX ctx;
    unsigned char digest[16];
    char output[33] = {0};

    MD5Init(&ctx);
    MD5Update(&ctx, const_cast<unsigned char *>(input), input_len);
    MD5Final(&ctx, digest);

    for (int i = 0; i < 16; i++) {
        snprintf(output + i * 2, 3, "%02x", digest[i]);
    }

    return std::string(output);
}

int check_user_pwd(const char *username, const char *pwd)
{
    char mysql_user[256] = {0};
    char mysql_pwd[256] = {0};
    char mysql_db[256] = {0};
    get_mysql_info(mysql_user, mysql_pwd, mysql_db);
    LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC,
            "mysql_user = %s, mysql_pwd = %s, mysql_db = %s\n",
            mysql_user, mysql_pwd, mysql_db);

    MysqlPtr conn(msql_conn(mysql_user, mysql_pwd, mysql_db));
    if (conn == nullptr) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "msql_conn err\n");
        return -1;
    }

    mysql_query(conn.get(), "set names utf8");

    char sql_cmd[SQL_MAX_LEN] = {0};
    char stored_hash[PWD_LEN] = {0};
    snprintf(sql_cmd, sizeof(sql_cmd),
            "select password from user_info where user_name='%s'", username);
    if (process_result_one(conn.get(), sql_cmd, stored_hash) != 0) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "user not found or query failed\n");
        return -1;
    }

    char salt[PWD_LEN] = {0};
    snprintf(sql_cmd, sizeof(sql_cmd),
            "select salt from user_info where user_name='%s'", username);
    if (process_result_one(conn.get(), sql_cmd, salt) != 0) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "salt query failed\n");
        return -1;
    }

    std::string salt_plus_pwd = std::string(salt) + pwd;
    std::string computed_hash = compute_md5_hex(
            reinterpret_cast<const unsigned char *>(salt_plus_pwd.c_str()),
            static_cast<unsigned int>(salt_plus_pwd.size()));

    return strcmp(stored_hash, computed_hash.c_str()) == 0 ? 0 : -1;
}

int set_token(const char *username, char *token, size_t token_size)
{
    char redis_ip[30] = {0};
    char redis_port[10] = {0};

    get_cfg_value(CFG_PATH, "redis", "ip", redis_ip);
    get_cfg_value(CFG_PATH, "redis", "port", redis_port);
    LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC,
            "redis:[ip=%s,port=%s]\n", redis_ip, redis_port);

    RedisPtr redis_conn(rop_connectdb_nopwd(redis_ip, redis_port), rop_disconnect);
    if (redis_conn == nullptr) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "redis connected error\n");
        return -1;
    }

    srand(static_cast<unsigned int>(time(nullptr)));
    char tmp[1024] = {0};
    snprintf(tmp, sizeof(tmp), "%s%d%d%d%d", username,
            rand() % 1000, rand() % 1000, rand() % 1000, rand() % 1000);
    LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "tmp = %s\n", tmp);

    char enc_tmp[1024 * 2] = {0};
    int enc_len = 0;
    int ret = DesEnc(reinterpret_cast<unsigned char *>(tmp), strlen(tmp),
            reinterpret_cast<unsigned char *>(enc_tmp), &enc_len);
    if (ret != 0) {
        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "DesEnc error\n");
        return -1;
    }

    char base64[1024 * 3] = {0};
    base64_encode(reinterpret_cast<const unsigned char *>(enc_tmp), enc_len, base64);
    LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "base64 = %s\n", base64);

    std::string md5_token = compute_md5_hex(
            reinterpret_cast<const unsigned char *>(base64),
            static_cast<unsigned int>(strlen(base64)));
    snprintf(token, token_size, "%s", md5_token.c_str());

    return rop_setex_string(redis_conn.get(), const_cast<char *>(username),
            86400, token);
}

void return_login_status(int ret_code, const char *token)
{
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    if (root == nullptr) {
        return;
    }

    cJSON_AddItemToObject(root.get(), "code", cJSON_CreateNumber(ret_code));
    cJSON_AddStringToObject(root.get(), "token", token);

    char *out = cJSON_Print(root.get());
    if (out != nullptr) {
        FCGI_printf("%s", out);
        free(out);
    }
}

} // namespace

int main()
{
    while (FCGI_Accept() >= 0) {
        char *content_length = getenv("CONTENT_LENGTH");
        int len = content_length == nullptr ? 0 : atoi(content_length);
        char token[128] = {0};

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0) {
            FCGI_printf("No data from standard input.<p>\n");
            LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC,
                    "len = 0, No data from standard input\n");
            continue;
        }

        char buf[4 * 1024] = {0};
        int read_len = FCGI_fread(buf, 1, len, stdin);
        if (read_len == 0) {
            LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC,
                    "fread(buf, 1, len, stdin) err\n");
            continue;
        }

        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "buf = %s\n", buf);

        char username[512] = {0};
        char pwd[512] = {0};
        if (get_login_info(buf, username, sizeof(username), pwd, sizeof(pwd)) != 0) {
            return_login_status(HTTP_RESP_FAIL, "fail");
            continue;
        }

        LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC,
                "username = %s, pwd = %s\n", username, pwd);

        int ret = check_user_pwd(username, pwd);
        if (ret == 0) {
            memset(token, 0, sizeof(token));
            ret = set_token(username, token, sizeof(token));
            LOG(LOGIN_LOG_MODULE, LOGIN_LOG_PROC, "token = %s\n", token);
        }

        if (ret == 0) {
            return_login_status(HTTP_RESP_OK, token);
        } else {
            return_login_status(HTTP_RESP_FAIL, "fail");
        }
    }

    return 0;
}
