/**
 * @file myfiles_cgi.cpp
 * @brief 用户文件列表展示 CGI 程序，C++ 版本
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

extern "C" {
#include "cfg.h"
#include "cJSON.h"
#include "deal_mysql.h"
#include "make_log.h"
#include "util_cgi.h"
}

#define MYFILES_LOG_MODULE "cgi"
#define MYFILES_LOG_PROC "myfiles"

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

struct MysqlResultDeleter {
    void operator()(MYSQL_RES *res) const
    {
        if (res != nullptr) {
            mysql_free_result(res);
        }
    }
};

using MysqlPtr = std::unique_ptr<MYSQL, MysqlDeleter>;
using MysqlResultPtr = std::unique_ptr<MYSQL_RES, MysqlResultDeleter>;

void read_cfg()
{
    get_cfg_value(CFG_PATH, "mysql", "user", mysql_user);
    get_cfg_value(CFG_PATH, "mysql", "password", mysql_pwd);
    get_cfg_value(CFG_PATH, "mysql", "database", mysql_db);
    LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
            "mysql:[user=%s,pwd=%s,database=%s]",
            mysql_user, mysql_pwd, mysql_db);
}

bool copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item == nullptr || item->valuestring == nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                "cJSON_GetObjectItem %s err\n", key);
        return false;
    }

    snprintf(dst, dst_size, "%s", item->valuestring);
    return true;
}

int get_count_json_info(const char *buf, char *user, size_t user_size,
        char *token, size_t token_size)
{
    JsonPtr root(cJSON_Parse(buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    if (!copy_json_string(root.get(), "user", user, user_size) ||
            !copy_json_string(root.get(), "token", token, token_size)) {
        return -1;
    }

    return 0;
}

void print_allocated_json(char *out)
{
    if (out != nullptr) {
        FCGI_printf("%s", out);
        free(out);
    }
}

void return_myfiles_status(long total, int ret_code)
{
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    if (root == nullptr) {
        return;
    }

    cJSON_AddItemToObject(root.get(), "code", cJSON_CreateNumber(ret_code));
    cJSON_AddItemToObject(root.get(), "total", cJSON_CreateNumber(total));

    print_allocated_json(cJSON_Print(root.get()));
}

int get_user_files_count(const char *user, long *count)
{
    char sql_cmd[SQL_MAX_LEN] = {0};
    long line = 0;
    int ret = 0;

    MysqlPtr conn(msql_conn(mysql_user, mysql_pwd, mysql_db));
    if (conn == nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "msql_conn err\n");
        *count = line;
        return -1;
    }

    mysql_query(conn.get(), "set names utf8");

    snprintf(sql_cmd, sizeof(sql_cmd),
            "select count from user_file_count where user='%s'", user);
    char tmp[512] = {0};
    ret = process_result_one(conn.get(), sql_cmd, tmp);
    if (ret == 1) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                "user '%s' has no record in user_file_count, count=0\n", user);
        line = 0;
        ret = 0;
    } else if (ret != 0) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "%s 操作失败\n", sql_cmd);
    } else {
        line = atol(tmp);
    }

    *count = line;
    LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
            "ret = %d, line = %ld\n", ret, line);
    return ret;
}

void handle_user_files_count(const char *user)
{
    long line = 0;
    int ret = get_user_files_count(user, &line);

    LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
            "ret = %d, line = %ld\n", ret, line);
    return_myfiles_status(ret == 0 ? line : 0,
            ret == 0 ? HTTP_RESP_OK : HTTP_RESP_FAIL);
}

int get_fileslist_json_info(const char *buf, char *user, size_t user_size,
        char *token, size_t token_size, int *p_start, int *p_count)
{
    JsonPtr root(cJSON_Parse(buf), cJSON_Delete);
    if (root == nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "cJSON_Parse err\n");
        return -1;
    }

    if (!copy_json_string(root.get(), "user", user, user_size) ||
            !copy_json_string(root.get(), "token", token, token_size)) {
        return -1;
    }

    cJSON *start_item = cJSON_GetObjectItem(root.get(), "start");
    cJSON *count_item = cJSON_GetObjectItem(root.get(), "count");
    if (start_item == nullptr || count_item == nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "start/count json err\n");
        return -1;
    }

    *p_start = start_item->valueint;
    *p_count = count_item->valueint;
    LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "count:%d\n", *p_count);

    return 0;
}

void add_row_string(cJSON *item, const char *key, MYSQL_ROW row, int index)
{
    if (row[index] != nullptr) {
        cJSON_AddStringToObject(item, key, row[index]);
    }
}

void add_row_number(cJSON *item, const char *key, MYSQL_ROW row, int index)
{
    if (row[index] != nullptr) {
        cJSON_AddNumberToObject(item, key, atol(row[index]));
    }
}

int get_user_filelist(const char *cmd, const char *user, int start, int count)
{
    int ret = 0;
    int line = 0;
    long total = 0;
    char sql_cmd[SQL_MAX_LEN] = {0};

    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    if (root == nullptr) {
        return -1;
    }

    ret = get_user_files_count(user, &total);
    if (ret != 0) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "get_user_files_count err\n");
        goto END;
    }

    {
        MysqlPtr conn(msql_conn(mysql_user, mysql_pwd, mysql_db));
        if (conn == nullptr) {
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "msql_conn err\n");
            ret = -1;
            goto END;
        }

        mysql_query(conn.get(), "set names utf8");

        if (strcmp(cmd, "normal") == 0) {
            snprintf(sql_cmd, sizeof(sql_cmd),
                    "select user_file_list.*, file_info.url, file_info.size, file_info.type "
                    "from file_info, user_file_list where user = '%s' "
                    "and file_info.md5 = user_file_list.md5 "
                    "order by user_file_list.create_time desc limit %d, %d",
                    user, start, count);
        } else if (strcmp(cmd, "pvasc") == 0) {
            snprintf(sql_cmd, sizeof(sql_cmd),
                    "select user_file_list.*, file_info.url, file_info.size, file_info.type "
                    "from file_info, user_file_list where user = '%s' "
                    "and file_info.md5 = user_file_list.md5 order by pv asc limit %d, %d",
                    user, start, count);
        } else if (strcmp(cmd, "pvdesc") == 0) {
            snprintf(sql_cmd, sizeof(sql_cmd),
                    "select user_file_list.*, file_info.url, file_info.size, file_info.type "
                    "from file_info, user_file_list where user = '%s' "
                    "and file_info.md5 = user_file_list.md5 order by pv desc limit %d, %d",
                    user, start, count);
        } else {
            ret = -1;
            goto END;
        }

        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "%s 在操作\n", sql_cmd);

        if (mysql_query(conn.get(), sql_cmd) != 0) {
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                    "%s 操作失败：%s\n", sql_cmd, mysql_error(conn.get()));
            ret = -1;
            goto END;
        }

        MysqlResultPtr res_set(mysql_store_result(conn.get()));
        if (res_set == nullptr) {
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                    "mysql_store_result error: %s!\n", mysql_error(conn.get()));
            ret = -1;
            goto END;
        }

        line = static_cast<int>(mysql_num_rows(res_set.get()));
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                "mysql_num_rows(res_set) = %d\n", line);

        JsonPtr array(cJSON_CreateArray(), cJSON_Delete);
        if (array == nullptr) {
            ret = -1;
            goto END;
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res_set.get())) != nullptr) {
            cJSON *item = cJSON_CreateObject();
            if (item == nullptr) {
                ret = -1;
                goto END;
            }

            add_row_string(item, "user", row, 1);
            add_row_string(item, "md5", row, 2);
            add_row_string(item, "create_time", row, 3);
            add_row_string(item, "file_name", row, 4);
            add_row_number(item, "share_status", row, 5);
            add_row_number(item, "pv", row, 6);
            add_row_string(item, "url", row, 7);
            add_row_number(item, "size", row, 8);
            add_row_string(item, "type", row, 9);

            cJSON_AddItemToArray(array.get(), item);
        }

        cJSON_AddItemToObject(root.get(), "files", array.release());
    }

END:
    cJSON_AddItemToObject(root.get(), "code",
            cJSON_CreateNumber(ret == 0 ? HTTP_RESP_OK : HTTP_RESP_FAIL));
    cJSON_AddItemToObject(root.get(), "count", cJSON_CreateNumber(line));
    cJSON_AddItemToObject(root.get(), "total", cJSON_CreateNumber(total));

    char *out = cJSON_Print(root.get());
    if (out != nullptr) {
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "%s\n", out);
    }
    print_allocated_json(out);

    return ret;
}

void print_fail_status()
{
    print_allocated_json(return_status(HTTP_RESP_FAIL));
}

} // namespace

int main()
{
    char cmd[20] = {0};
    char user[USER_NAME_LEN] = {0};
    char token[TOKEN_LEN] = {0};

    read_cfg();

    while (FCGI_Accept() >= 0) {
        memset(cmd, 0, sizeof(cmd));
        memset(user, 0, sizeof(user));
        memset(token, 0, sizeof(token));

        char *query = getenv("QUERY_STRING");
        query_parse_key_value(query, "cmd", cmd, nullptr);
        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "cmd = %s\n", cmd);

        char *content_length = getenv("CONTENT_LENGTH");
        int len = content_length == nullptr ? 0 : atoi(content_length);

        FCGI_printf("Content-type: text/html\r\n\r\n");

        if (len <= 0) {
            FCGI_printf("No data from standard input.<p>\n");
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                    "len = 0, No data from standard input\n");
            continue;
        }

        char buf[4 * 1024] = {0};
        int ret = FCGI_fread(buf, 1, len, stdin);
        if (ret == 0) {
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                    "fread(buf, 1, len, stdin) err\n");
            continue;
        }

        LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC, "buf = %s\n", buf);

        if (strcmp(cmd, "count") == 0) {
            if (get_count_json_info(buf, user, sizeof(user),
                    token, sizeof(token)) != 0) {
                print_fail_status();
                continue;
            }

            ret = verify_token(user, token);
            if (ret == 0) {
                handle_user_files_count(user);
            } else {
                print_fail_status();
            }
        } else {
            int start = 0;
            int count = 0;
            ret = get_fileslist_json_info(buf, user, sizeof(user),
                    token, sizeof(token), &start, &count);
            LOG(MYFILES_LOG_MODULE, MYFILES_LOG_PROC,
                    "user = %s, token = %s, start = %d, count = %d\n",
                    user, token, start, count);
            if (ret != 0) {
                print_fail_status();
                continue;
            }

            ret = verify_token(user, token);
            if (ret == 0) {
                get_user_filelist(cmd, user, start, count);
            } else {
                print_fail_status();
            }
        }
    }

    return 0;
}
