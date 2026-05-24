/**
 * @file reg_cgi.cpp
 * @brief 注册事件后 CGI 程序，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

extern "C" {
#include "cfg.h"
#include "cJSON.h"
#include "deal_mysql.h"
#include "make_log.h"
#include "md5.h"
#include "util_cgi.h"
}

#define REG_LOG_MODULE "cgi"
#define REG_LOG_PROC   "reg"

namespace {

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

struct MysqlDeleter {
    void operator()(MYSQL *conn) const
    {
        if (conn != nullptr) {
            mysql_close(conn);
        }
    }
};

using MysqlPtr = std::unique_ptr<MYSQL, MysqlDeleter>;

bool copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == nullptr || item->valuestring == nullptr) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC, "cJSON_GetObjectItem %s err\n", key);
        return false;
    }

    snprintf(dst, dst_size, "%s", item->valuestring);
    return true;
}

int get_reg_info(const char *reg_buf, char *user, size_t user_size,
        char *nick_name, size_t nick_size, char *pwd, size_t pwd_size,
        char *tel, size_t tel_size, char *email, size_t email_size)
{
    JsonPtr root(cJSON_Parse(reg_buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    if (!copy_json_string(root.get(), "userName", user, user_size) ||
            !copy_json_string(root.get(), "nickName", nick_name, nick_size) ||
            !copy_json_string(root.get(), "firstPwd", pwd, pwd_size) ||
            !copy_json_string(root.get(), "phone", tel, tel_size) ||
            !copy_json_string(root.get(), "email", email, email_size)) {
        return -1;
    }

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

void generate_salt(char *out_salt, size_t salt_size)
{
    unsigned char raw[8] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t ignored = read(fd, raw, sizeof(raw));
        (void)ignored;
        close(fd);
    } else {
        srand(static_cast<unsigned int>(time(nullptr)));
        for (int i = 0; i < 8; i++) {
            raw[i] = static_cast<unsigned char>(rand() % 256);
        }
    }

    char tmp[17] = {0};
    for (int i = 0; i < 8; i++) {
        snprintf(tmp + i * 2, 3, "%02x", raw[i]);
    }
    snprintf(out_salt, salt_size, "%s", tmp);
}

int user_register(const char *reg_buf)
{
    char mysql_user[256] = {0};
    char mysql_pwd[256] = {0};
    char mysql_db[256] = {0};
    if (get_mysql_info(mysql_user, mysql_pwd, mysql_db) != 0) {
        return -1;
    }
    LOG(REG_LOG_MODULE, REG_LOG_PROC,
            "mysql_user = %s, mysql_pwd = %s, mysql_db = %s\n",
            mysql_user, mysql_pwd, mysql_db);

    char user_name[128] = {0};
    char nick_name[128] = {0};
    char pwd[128] = {0};
    char tel[128] = {0};
    char email[128] = {0};
    if (get_reg_info(reg_buf, user_name, sizeof(user_name),
            nick_name, sizeof(nick_name), pwd, sizeof(pwd),
            tel, sizeof(tel), email, sizeof(email)) != 0) {
        return -1;
    }

    LOG(REG_LOG_MODULE, REG_LOG_PROC,
            "user_name = %s, nick_name = %s, pwd = %s, tel = %s, email = %s\n",
            user_name, nick_name, pwd, tel, email);

    MysqlPtr conn(msql_conn(mysql_user, mysql_pwd, mysql_db));
    if (conn == nullptr) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC, "msql_conn err\n");
        return -1;
    }

    mysql_query(conn.get(), "set names utf8");

    char sql_cmd[SQL_MAX_LEN] = {0};
    snprintf(sql_cmd, sizeof(sql_cmd),
            "select * from user_info where user_name = '%s'", user_name);
    int ret2 = process_result_one(conn.get(), sql_cmd, nullptr);
    if (ret2 == 2) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC, "【%s】该用户已存在\n", user_name);
        return -2;
    }

    snprintf(sql_cmd, sizeof(sql_cmd),
            "select * from user_info where nick_name = '%s'", nick_name);
    ret2 = process_result_one(conn.get(), sql_cmd, nullptr);
    if (ret2 == 2) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC, "【%s】该昵称已存在\n", nick_name);
        return -3;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm *ptm = localtime(&tv.tv_sec);
    char time_str[128] = {0};
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", ptm);

    char salt[33] = {0};
    generate_salt(salt, sizeof(salt));

    std::string salt_plus_pwd = std::string(salt) + pwd;
    std::string salted_hash = compute_md5_hex(
            reinterpret_cast<const unsigned char *>(salt_plus_pwd.c_str()),
            static_cast<unsigned int>(salt_plus_pwd.size()));

    snprintf(sql_cmd, sizeof(sql_cmd),
            "insert into user_info (user_name, nick_name, password, salt, phone, email, create_time) "
            "values ('%s', '%s', '%s', '%s', '%s', '%s', '%s')",
            user_name, nick_name, salted_hash.c_str(), salt, tel, email, time_str);

    if (mysql_query(conn.get(), sql_cmd) != 0) {
        LOG(REG_LOG_MODULE, REG_LOG_PROC,
                "%s 插入失败：%s\n", sql_cmd, mysql_error(conn.get()));
        return -1;
    }

    return 0;
}

void print_allocated_status(char *out)
{
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

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0) {
            FCGI_printf("No data from standard input.<p>\n");
            LOG(REG_LOG_MODULE, REG_LOG_PROC,
                    "len = 0, No data from standard input\n");
            continue;
        }

        char buf[4 * 1024] = {0};
        int ret = FCGI_fread(buf, 1, len, stdin);
        if (ret == 0) {
            LOG(REG_LOG_MODULE, REG_LOG_PROC,
                    "fread(buf, 1, len, stdin) err\n");
            continue;
        }

        LOG(REG_LOG_MODULE, REG_LOG_PROC, "buf = %s\n", buf);

        ret = user_register(buf);
        if (ret == 0) {
            print_allocated_status(return_status(HTTP_RESP_OK));
        } else if (ret == -2) {
            print_allocated_status(return_status(HTTP_RESP_USER_EXIST));
        } else if (ret == -3) {
            print_allocated_status(return_status(HTTP_RESP_NICK_EXIST));
        } else {
            print_allocated_status(return_status(HTTP_RESP_FAIL));
        }
    }

    return 0;
}
