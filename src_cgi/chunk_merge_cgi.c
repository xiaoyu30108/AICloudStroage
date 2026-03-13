/**
 * @file chunk_merge_cgi.c
 * @brief  分片合并CGI程序
 *         收到合并请求后：
 *         1. 验证所有分片是否完整
 *         2. 使用FastDFS appender API依次上传并合并分片
 *         3. 每个分片合并后立即删除
 *         4. 将文件信息写入MySQL
 *         5. 清理Redis记录和临时目录
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include "make_log.h"
#include "util_cgi.h"
#include "deal_mysql.h"
#include "cfg.h"
#include "cJSON.h"
#include "redis_op.h"
#include "fdfs_client.h"

#define CHUNK_LOG_MODULE  "cgi"
#define CHUNK_LOG_PROC    "chunk_merge"
#define CHUNK_TEMP_DIR    "/tmp/chunks"

static char mysql_user[128] = {0};
static char mysql_pwd[128] = {0};
static char mysql_db[128] = {0};
static char redis_ip[30] = {0};
static char redis_port[10] = {0};

void read_cfg()
{
    get_cfg_value(CFG_PATH, "mysql", "user", mysql_user);
    get_cfg_value(CFG_PATH, "mysql", "password", mysql_pwd);
    get_cfg_value(CFG_PATH, "mysql", "database", mysql_db);
    get_cfg_value(CFG_PATH, "redis", "ip", redis_ip);
    get_cfg_value(CFG_PATH, "redis", "port", redis_port);
    LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
        "mysql:[user=%s,pwd=%s,db=%s], redis:[%s:%s]",
        mysql_user, mysql_pwd, mysql_db, redis_ip, redis_port);
}

int parse_merge_json(char *buf, char *user, char *token, char *file_md5, char *filename)
{
    int ret = 0;
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) return -1;

    cJSON *item;

    item = cJSON_GetObjectItem(root, "user");
    if (!item) { ret = -1; goto END; }
    strcpy(user, item->valuestring);

    item = cJSON_GetObjectItem(root, "token");
    if (!item) { ret = -1; goto END; }
    strcpy(token, item->valuestring);

    item = cJSON_GetObjectItem(root, "md5");
    if (!item) { ret = -1; goto END; }
    strcpy(file_md5, item->valuestring);

    item = cJSON_GetObjectItem(root, "filename");
    if (!item) { ret = -1; goto END; }
    strcpy(filename, item->valuestring);

END:
    cJSON_Delete(root);
    return ret;
}

// 验证所有分片是否存在
int verify_all_chunks(char *file_md5, int chunk_count)
{
    int i;
    char path[512];
    struct stat st;

    for (i = 0; i < chunk_count; i++)
    {
        sprintf(path, "%s/%s/%d", CHUNK_TEMP_DIR, file_md5, i);
        if (stat(path, &st) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "chunk %d missing: %s\n", i, path);
            return -1;
        }
    }
    return 0;
}

// 使用FastDFS appender API合并分片
int merge_chunks_to_fastdfs(char *file_md5, int chunk_count, char *filename, char *fileid)
{
    int result = 0;
    char fdfs_cli_conf_path[256] = {0};
    char path[512] = {0};
    struct stat st;
    char *buf = NULL;
    int fd;
    char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
    ConnectionInfo *pTrackerServer = NULL;
    ConnectionInfo storageServer;
    int store_path_index;

    // 获取文件后缀
    char suffix[SUFFIX_LEN] = {0};
    get_file_suffix(filename, suffix);

    // 加载FastDFS配置
    get_cfg_value(CFG_PATH, "dfs_path", "client", fdfs_cli_conf_path);

    log_init();
    g_log_context.log_level = LOG_ERR;
    ignore_signal_pipe();

    result = fdfs_client_init(fdfs_cli_conf_path);
    if (result != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "fdfs_client_init err: %d\n", result);
        return -1;
    }

    pTrackerServer = tracker_get_connection();
    if (pTrackerServer == NULL)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "tracker_get_connection err\n");
        fdfs_client_destroy();
        return -1;
    }

    *group_name = '\0';
    result = tracker_query_storage_store(pTrackerServer,
                                         &storageServer, group_name, &store_path_index);
    if (result != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "tracker_query_storage_store err: %d\n", result);
        fdfs_client_destroy();
        return -1;
    }

    // ====== 上传第一个分片(index=0)作为appender文件 ======
    sprintf(path, "%s/%s/0", CHUNK_TEMP_DIR, file_md5);
    if (stat(path, &st) != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "stat chunk 0 err\n");
        fdfs_client_destroy();
        return -1;
    }

    buf = (char *)malloc(st.st_size);
    if (buf == NULL)
    {
        fdfs_client_destroy();
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        free(buf);
        fdfs_client_destroy();
        return -1;
    }

    long total = 0;
    while (total < st.st_size)
    {
        int n = read(fd, buf + total, st.st_size - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    // 使用appender API上传第一个分片
    result = storage_upload_appender_by_filebuff1(
        pTrackerServer, &storageServer, store_path_index,
        buf, st.st_size,
        suffix,
        NULL, 0,
        group_name, fileid);

    free(buf);
    unlink(path); // 删除分片0

    if (result != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "upload appender chunk 0 err: %d\n", result);
        tracker_close_connection_ex(pTrackerServer, true);
        fdfs_client_destroy();
        return -1;
    }

    LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
        "appender file created: %s\n", fileid);

    // ====== 追加后续分片(index 1 ~ chunk_count-1) ======
    int i;
    for (i = 1; i < chunk_count; i++)
    {
        sprintf(path, "%s/%s/%d", CHUNK_TEMP_DIR, file_md5, i);
        if (stat(path, &st) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "stat chunk %d err\n", i);
            result = -1;
            break;
        }

        buf = (char *)malloc(st.st_size);
        if (buf == NULL) { result = -1; break; }

        fd = open(path, O_RDONLY);
        if (fd < 0) { free(buf); result = -1; break; }

        total = 0;
        while (total < st.st_size)
        {
            int n = read(fd, buf + total, st.st_size - total);
            if (n <= 0) break;
            total += n;
        }
        close(fd);

        // 追加到appender文件
        result = storage_append_by_filebuff1(
            pTrackerServer, &storageServer,
            fileid,
            buf, st.st_size);

        free(buf);
        unlink(path); // 合并后立即删除该分片

        if (result != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
                "append chunk %d err: %d\n", i, result);
            break;
        }

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "appended chunk %d (%ld bytes)\n", i, (long)st.st_size);
    }

    tracker_close_connection_ex(pTrackerServer, true);
    fdfs_client_destroy();

    // 删除临时目录
    char chunk_dir[512] = {0};
    sprintf(chunk_dir, "%s/%s", CHUNK_TEMP_DIR, file_md5);
    rmdir(chunk_dir);

    return result;
}

// 构造文件URL（复用upload_cgi.c的逻辑）
int make_file_url(char *fileid, char *fdfs_file_url)
{
    char fdfs_file_host_name[HOST_NAME_LEN] = {0};
    char storage_web_server_port[20] = {0};

    get_cfg_value(CFG_PATH, "storage_web_server", "port", storage_web_server_port);
    get_cfg_value(CFG_PATH, "storage_web_server", "ip", fdfs_file_host_name);

    strcat(fdfs_file_url, "http://");
    strcat(fdfs_file_url, fdfs_file_host_name);
    strcat(fdfs_file_url, ":");
    strcat(fdfs_file_url, storage_web_server_port);
    strcat(fdfs_file_url, "/");
    strcat(fdfs_file_url, fileid);

    LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "file url: %s\n", fdfs_file_url);
    return 0;
}

// 存储文件信息到MySQL（复用upload_cgi.c的逻辑）
int store_fileinfo_to_mysql(char *user, char *filename, char *md5,
                            long size, char *fileid, char *fdfs_file_url)
{
    int ret = 0;
    MYSQL *conn = NULL;
    time_t now;
    char create_time[TIME_STRING_LEN];
    char suffix[SUFFIX_LEN];
    char sql_cmd[SQL_MAX_LEN] = {0};

    conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
    if (conn == NULL)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "mysql connect err\n");
        return -1;
    }

    mysql_query(conn, "set names utf8");

    get_file_suffix(filename, suffix);

    sprintf(sql_cmd,
        "insert into file_info (md5, file_id, url, size, type, count) "
        "values ('%s', '%s', '%s', '%ld', '%s', %d)",
        md5, fileid, fdfs_file_url, size, suffix, 1);

    if (mysql_query(conn, sql_cmd) != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "%s err: %s\n", sql_cmd, mysql_error(conn));
        ret = -1;
        goto END;
    }

    now = time(NULL);
    strftime(create_time, TIME_STRING_LEN - 1, "%Y-%m-%d %H:%M:%S", localtime(&now));

    sprintf(sql_cmd,
        "insert into user_file_list(user, md5, create_time, file_name, shared_status, pv) "
        "values ('%s', '%s', '%s', '%s', %d, %d)",
        user, md5, create_time, filename, 0, 0);

    if (mysql_query(conn, sql_cmd) != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "%s err: %s\n", sql_cmd, mysql_error(conn));
        ret = -1;
        goto END;
    }

    // 更新用户文件数量
    sprintf(sql_cmd, "select count from user_file_count where user = '%s'", user);
    char tmp[512] = {0};
    int ret2 = process_result_one(conn, sql_cmd, tmp);
    if (ret2 == 1)
    {
        sprintf(sql_cmd,
            "insert into user_file_count (user, count) values('%s', %d)", user, 1);
    }
    else if (ret2 == 0)
    {
        int count = atoi(tmp);
        sprintf(sql_cmd,
            "update user_file_count set count = %d where user = '%s'", count + 1, user);
    }

    if (mysql_query(conn, sql_cmd) != 0)
    {
        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "%s err: %s\n", sql_cmd, mysql_error(conn));
        ret = -1;
        goto END;
    }

END:
    if (conn != NULL)
        mysql_close(conn);
    return ret;
}

int main()
{
    read_cfg();

    while (FCGI_Accept() >= 0)
    {
        char *contentLength = getenv("CONTENT_LENGTH");
        int len = 0;

        printf("Content-type: text/html\r\n\r\n");

        if (contentLength != NULL)
            len = atoi(contentLength);

        if (len <= 0)
        {
            printf("{\"code\":1}");
            continue;
        }

        char buf[4 * 1024] = {0};
        int ret = fread(buf, 1, len, stdin);
        if (ret == 0)
        {
            printf("{\"code\":1}");
            continue;
        }

        char user[128] = {0};
        char token[256] = {0};
        char file_md5[256] = {0};
        char filename[256] = {0};

        if (parse_merge_json(buf, user, token, file_md5, filename) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "parse json err\n");
            printf("{\"code\":1}");
            continue;
        }

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "merge request: user=%s, md5=%s, filename=%s\n",
            user, file_md5, filename);

        // 验证token
        ret = verify_token(user, token);
        if (ret != 0)
        {
            printf("{\"code\":4}");
            continue;
        }

        // 从Redis获取分片信息
        redisContext *redis_conn = rop_connectdb_nopwd(redis_ip, redis_port);
        if (redis_conn == NULL)
        {
            printf("{\"code\":1}");
            continue;
        }

        char redis_key[512] = {0};
        sprintf(redis_key, "chunk:%s", file_md5);

        char count_str[32] = {0};
        char size_str[64] = {0};

        if (rop_hash_get(redis_conn, redis_key, "chunk_count", count_str) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "get chunk_count from redis err\n");
            rop_disconnect(redis_conn);
            printf("{\"code\":1}");
            continue;
        }

        if (rop_hash_get(redis_conn, redis_key, "filesize", size_str) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "get filesize from redis err\n");
            rop_disconnect(redis_conn);
            printf("{\"code\":1}");
            continue;
        }

        int chunk_count = atoi(count_str);
        long filesize = strtol(size_str, NULL, 10);

        // 验证分片完整性
        if (verify_all_chunks(file_md5, chunk_count) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "chunks incomplete\n");
            rop_disconnect(redis_conn);
            printf("{\"code\":1,\"msg\":\"chunks incomplete\"}");
            continue;
        }

        // 合并分片到FastDFS
        char fileid[TEMP_BUF_MAX_LEN] = {0};
        if (merge_chunks_to_fastdfs(file_md5, chunk_count, filename, fileid) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "merge to fastdfs err\n");
            rop_disconnect(redis_conn);
            printf("{\"code\":1,\"msg\":\"merge failed\"}");
            continue;
        }

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "merge OK, fileid=%s\n", fileid);

        // 构造文件URL
        char fdfs_file_url[FILE_URL_LEN] = {0};
        make_file_url(fileid, fdfs_file_url);

        // 存入MySQL
        if (store_fileinfo_to_mysql(user, filename, file_md5, filesize, fileid, fdfs_file_url) != 0)
        {
            LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC, "store to mysql err\n");
            rop_disconnect(redis_conn);
            printf("{\"code\":1,\"msg\":\"db error\"}");
            continue;
        }

        // 清理Redis
        rop_del_key(redis_conn, redis_key);
        rop_disconnect(redis_conn);

        LOG(CHUNK_LOG_MODULE, CHUNK_LOG_PROC,
            "chunk merge complete: %s -> %s\n", filename, fdfs_file_url);

        printf("{\"code\":0}");
    }

    return 0;
}
