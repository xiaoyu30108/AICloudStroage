/**
 * @file md5_cgi.cpp
 * @brief 秒传功能 CGI，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/time.h>

extern "C" {
#include "cfg.h"
#include "cJSON.h"
#include "deal_mysql.h"
#include "make_log.h"
#include "util_cgi.h"
}

#define MD5_LOG_MODULE "cgi"
#define MD5_LOG_PROC   "md5"

namespace {

char mysql_user[128] = {0};
char mysql_pwd[128] = {0};
char mysql_db[128] = {0};

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

void read_cfg()
{
    get_cfg_value(CFG_PATH, "mysql", "user", mysql_user);
    get_cfg_value(CFG_PATH, "mysql", "password", mysql_pwd);
    get_cfg_value(CFG_PATH, "mysql", "database", mysql_db);
    LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
            "mysql:[user=%s,pwd=%s,database=%s]",
            mysql_user, mysql_pwd, mysql_db);
}

bool copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == nullptr || item->valuestring == nullptr) {
        LOG(MD5_LOG_MODULE, MD5_LOG_PROC, "cJSON_GetObjectItem %s err\n", key);
        return false;
    }

    snprintf(dst, dst_size, "%s", item->valuestring);
    return true;
}

int get_md5_info(const char *buf, char *user, size_t user_size,
        char *token, size_t token_size, char *md5, size_t md5_size,
        char *filename, size_t filename_size)
{
    JsonPtr root(cJSON_Parse(buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(MD5_LOG_MODULE, MD5_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    if (!copy_json_string(root.get(), "user", user, user_size) ||
            !copy_json_string(root.get(), "md5", md5, md5_size) ||
            !copy_json_string(root.get(), "fileName", filename, filename_size) ||
            !copy_json_string(root.get(), "token", token, token_size)) {
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

void print_md5_result(int ret)
{
    if (ret == 0) {
        print_allocated_status(return_status(HTTP_RESP_OK));
    } else if (ret == -2) {
        print_allocated_status(return_status(HTTP_RESP_FILE_EXIST));
    } else {
        print_allocated_status(return_status(HTTP_RESP_FAIL));
    }
}

int deal_md5(const char *user, const char *md5, const char *filename)
{
    int ret = 0;
    int ret2 = 0;
    char tmp[512] = {0};
    char sql_cmd[SQL_MAX_LEN] = {0};
    char escaped_user[USER_NAME_LEN * 2 + 1] = {0};
    char escaped_filename[FILE_NAME_LEN * 2 + 1] = {0};

    MysqlPtr conn(msql_conn(mysql_user, mysql_pwd, mysql_db));
    if (conn == nullptr) {
        LOG(MD5_LOG_MODULE, MD5_LOG_PROC, "msql_conn err\n");
        print_md5_result(-1);
        return -1;
    }

    mysql_query(conn.get(), "set names utf8");
    if (sql_escape_string(conn.get(), user, escaped_user, sizeof(escaped_user)) != 0 ||
            sql_escape_string(conn.get(), filename, escaped_filename, sizeof(escaped_filename)) != 0) {
        print_md5_result(-1);
        return -1;
    }

    snprintf(sql_cmd, sizeof(sql_cmd),
            "select count from file_info where md5 = '%s'", md5);
    ret2 = process_result_one(conn.get(), sql_cmd, tmp);

    if (ret2 == 0) {
        int count = atoi(tmp);

        snprintf(sql_cmd, sizeof(sql_cmd),
                "select * from user_file_list where user = '%s' and md5 = '%s' and file_name = '%s'",
                escaped_user, md5, escaped_filename);
        ret2 = process_result_one(conn.get(), sql_cmd, nullptr);
        if (ret2 == 2) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "%s[filename:%s, md5:%s]已存在\n", user, filename, md5);
            ret = -2;
            goto END;
        }

        snprintf(sql_cmd, sizeof(sql_cmd),
                "update file_info set count = %d where md5 = '%s'", ++count, md5);
        if (mysql_query(conn.get(), sql_cmd) != 0) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "%s 操作失败： %s\n", sql_cmd, mysql_error(conn.get()));
            ret = -1;
            goto END;
        }

        struct timeval tv;
        gettimeofday(&tv, nullptr);
        struct tm *ptm = localtime(&tv.tv_sec);
        char time_str[128] = {0};
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", ptm);

        snprintf(sql_cmd, sizeof(sql_cmd),
                "insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) "
                "values ('%s', '%s', '%s', '%s', %d, %d)",
                escaped_user, md5, time_str, escaped_filename, 0, 0);
        if (mysql_query(conn.get(), sql_cmd) != 0) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "%s 操作失败： %s\n", sql_cmd, mysql_error(conn.get()));
            ret = -1;
            goto END;
        }

        snprintf(sql_cmd, sizeof(sql_cmd),
                "select count from user_file_count where user = '%s'", escaped_user);
        count = 0;
        ret2 = process_result_one(conn.get(), sql_cmd, tmp);
        if (ret2 == 1) {
            snprintf(sql_cmd, sizeof(sql_cmd),
                    "insert into user_file_count (user, count) values('%s', %d)",
                    escaped_user, 1);
        } else if (ret2 == 0) {
            count = atoi(tmp);
            snprintf(sql_cmd, sizeof(sql_cmd),
                    "update user_file_count set count = %d where user = '%s'",
                    count + 1, escaped_user);
        } else {
            ret = -1;
            goto END;
        }

        if (mysql_query(conn.get(), sql_cmd) != 0) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "%s 操作失败： %s\n", sql_cmd, mysql_error(conn.get()));
            ret = -1;
            goto END;
        }
    } else if (ret2 == 1) {
        ret = -3;
    } else {
        ret = -1;
    }

END:
    print_md5_result(ret);
    return ret;
}

} // namespace

int main()
{
    read_cfg();

    while (FCGI_Accept() >= 0) {
        char *content_length = getenv("CONTENT_LENGTH");
        int len = content_length == nullptr ? 0 : atoi(content_length);

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0) {
            FCGI_printf("No data from standard input.<p>\n");
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "len = 0, No data from standard input\n");
            continue;
        }

        char buf[4 * 1024] = {0};
        int ret = FCGI_fread(buf, 1, len, stdin);
        if (ret == 0) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                    "fread(buf, 1, len, stdin) err\n");
            continue;
        }

        LOG(MD5_LOG_MODULE, MD5_LOG_PROC, "buf = %s\n", buf);

        char user[128] = {0};
        char md5[256] = {0};
        char token[256] = {0};
        char filename[128] = {0};
        ret = get_md5_info(buf, user, sizeof(user), token, sizeof(token),
                md5, sizeof(md5), filename, sizeof(filename));
        if (ret != 0) {
            LOG(MD5_LOG_MODULE, MD5_LOG_PROC, "get_md5_info() err\n");
            print_allocated_status(return_status(HTTP_RESP_FAIL));
            continue;
        }

        LOG(MD5_LOG_MODULE, MD5_LOG_PROC,
                "user = %s, token = %s, md5 = %s, filename = %s\n",
                user, token, md5, filename);

        ret = verify_token(user, token);
        if (ret == 0) {
            deal_md5(user, md5, filename);
        } else {
            print_allocated_status(return_status(HTTP_RESP_TOKEN_ERR));
        }
    }

    return 0;
}
