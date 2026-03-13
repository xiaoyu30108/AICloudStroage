/**
 * @file ai_cgi.cpp
 * @brief  AI 智能检索 FastCGI 程序
 *         功能：describe（生成文件描述+向量）、search（语义搜索）、rebuild（重建索引）
 */

#include "fcgi_config.h"
#include "fcgi_stdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

extern "C" {
#include "make_log.h"
#include "util_cgi.h"
#include "deal_mysql.h"
#include "redis_keys.h"
#include "redis_op.h"
#include "cfg.h"
#include "cJSON.h"
}

#include "dashscope_api.h"
#include "faiss_wrapper.h"

#define AI_LOG_MODULE "cgi"
#define AI_LOG_PROC   "ai"

// 全局配置
static char mysql_user[128] = {0};
static char mysql_pwd[128] = {0};
static char mysql_db[128] = {0};
static char redis_ip[30] = {0};
static char redis_port[10] = {0};
static char dashscope_api_key[256] = {0};
static char embedding_model[64] = {0};
static char vl_model[64] = {0};
static int  embedding_dimension = 1024;
static char faiss_index_path[512] = {0};
static char web_server_ip[30] = {0};
static char web_server_port[10] = {0};
static char public_server_ip[30] = {0};
static char public_server_port[10] = {0};

static void read_cfg()
{
    get_cfg_value(CFG_PATH, "mysql", "user", mysql_user);
    get_cfg_value(CFG_PATH, "mysql", "password", mysql_pwd);
    get_cfg_value(CFG_PATH, "mysql", "database", mysql_db);

    get_cfg_value(CFG_PATH, "redis", "ip", redis_ip);
    get_cfg_value(CFG_PATH, "redis", "port", redis_port);

    get_cfg_value(CFG_PATH, "dashscope", "api_key", dashscope_api_key);
    get_cfg_value(CFG_PATH, "dashscope", "embedding_model", embedding_model);
    get_cfg_value(CFG_PATH, "dashscope", "vl_model", vl_model);

    char dim_str[16] = {0};
    get_cfg_value(CFG_PATH, "dashscope", "embedding_dimension", dim_str);
    if (strlen(dim_str) > 0) embedding_dimension = atoi(dim_str);

    get_cfg_value(CFG_PATH, "faiss", "index_path", faiss_index_path);

    get_cfg_value(CFG_PATH, "web_server", "ip", web_server_ip);
    get_cfg_value(CFG_PATH, "web_server", "port", web_server_port);

    get_cfg_value(CFG_PATH, "public_server", "ip", public_server_ip);
    get_cfg_value(CFG_PATH, "public_server", "port", public_server_port);

    LOG(AI_LOG_MODULE, AI_LOG_PROC, "config loaded: mysql=[%s], redis=[%s:%s], dim=%d, index=%s, public=%s:%s\n",
        mysql_db, redis_ip, redis_port, embedding_dimension, faiss_index_path, public_server_ip, public_server_port);
}

// 判断是否是图片类型
static int is_image_type(const char *type)
{
    if (!type) return 0;
    return (strcasecmp(type, "png") == 0 ||
            strcasecmp(type, "jpg") == 0 ||
            strcasecmp(type, "jpeg") == 0 ||
            strcasecmp(type, "gif") == 0 ||
            strcasecmp(type, "bmp") == 0 ||
            strcasecmp(type, "webp") == 0 ||
            strcasecmp(type, "svg") == 0);
}

// curl 写回调：用 open/write 绕过 fcgi_stdio 对 FILE 的重定义
static size_t download_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    int fd = *(int *)userdata;
    size_t total = size * nmemb;
    ssize_t written = write(fd, ptr, total);
    return (written > 0) ? (size_t)written : 0;
}

// 下载文件到本地临时路径
static int download_file(const char *url, const char *save_path)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    int fd = open(save_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { curl_easy_cleanup(curl); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fd);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    close(fd);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        remove(save_path);
        return -1;
    }
    return 0;
}

// 从 docx 文件提取纯文本（docx 是 ZIP，内含 word/document.xml）
// 用 pipe+fork+exec 代替 popen 以避免 fcgi_stdio 重定义
static int extract_docx_text(const char *docx_path, char *out_text, int max_len)
{
    char cmd[1024] = {0};
    snprintf(cmd, sizeof(cmd), "unzip -p '%s' word/document.xml 2>/dev/null", docx_path);

    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        // 子进程
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    }

    // 父进程
    close(pipefd[1]);

    // 读取 XML 内容
    char *xml_buf = (char *)malloc(256 * 1024); // 256KB
    if (!xml_buf) { close(pipefd[0]); return -1; }

    int total = 0;
    int n;
    while ((n = read(pipefd[0], xml_buf + total, 256 * 1024 - total - 1)) > 0) {
        total += n;
        if (total >= 256 * 1024 - 1) break;
    }
    xml_buf[total] = '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (total == 0) { free(xml_buf); return -1; }

    // 从 XML 中提取 <w:t> 或 <w:t ...> 标签内的文本
    // 注意：必须精确匹配 <w:t> 或 <w:t + 空格，避免匹配 <w:tab>、<w:tabs> 等
    int out_pos = 0;
    char *p = xml_buf;
    while (*p && out_pos < max_len - 2) {
        char *tag_start = strstr(p, "<w:t");
        if (!tag_start) break;

        // 检查 <w:t 后面是 > 或空格（表示属性），排除 <w:tab 等
        char next_ch = tag_start[4]; // "<w:t" 之后的字符
        if (next_ch != '>' && next_ch != ' ') {
            p = tag_start + 4;
            continue;
        }

        // 找到 > 结束
        char *content_start = strchr(tag_start, '>');
        if (!content_start) break;
        content_start++;

        // 找到 </w:t>
        char *content_end = strstr(content_start, "</w:t>");
        if (!content_end) break;

        int len = content_end - content_start;
        if (len > 0 && out_pos + len < max_len - 1) {
            memcpy(out_text + out_pos, content_start, len);
            out_pos += len;
        }

        p = content_end + 6;
    }
    out_text[out_pos] = '\0';

    free(xml_buf);
    return (out_pos > 0) ? 0 : -1;
}

// 读取纯文本文件（用 open/read 绕过 fcgi_stdio）
static int read_text_file(const char *path, char *out_text, int max_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    int n = read(fd, out_text, max_len - 1);
    close(fd);
    if (n <= 0) return -1;
    out_text[n] = '\0';
    return 0;
}

// 判断是否为文本类文件
static int is_text_type(const char *type)
{
    if (!type) return 0;
    return (strcasecmp(type, "txt") == 0 ||
            strcasecmp(type, "md") == 0 ||
            strcasecmp(type, "csv") == 0 ||
            strcasecmp(type, "json") == 0 ||
            strcasecmp(type, "xml") == 0 ||
            strcasecmp(type, "html") == 0 ||
            strcasecmp(type, "htm") == 0 ||
            strcasecmp(type, "log") == 0 ||
            strcasecmp(type, "c") == 0 ||
            strcasecmp(type, "cpp") == 0 ||
            strcasecmp(type, "h") == 0 ||
            strcasecmp(type, "py") == 0 ||
            strcasecmp(type, "js") == 0 ||
            strcasecmp(type, "css") == 0 ||
            strcasecmp(type, "java") == 0);
}

/**
 * 从数据库获取用户的 API Key
 * 返回 0 成功，-1 失败，out_key 填入 api_key
 */
static int get_user_apikey_from_db(const char *username, char *out_key, int max_len)
{
    MYSQL *conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
    if (!conn) return -1;
    mysql_query(conn, "set names utf8mb4");

    char sql[512] = {0};
    snprintf(sql, sizeof(sql), "SELECT api_key FROM user_info WHERE user_name='%s'", username);

    char tmp[256] = {0};
    int ret = process_result_one(conn, sql, tmp);
    mysql_close(conn);

    if (ret == 0 && strlen(tmp) > 0) {
        strncpy(out_key, tmp, max_len - 1);
        return 0;
    }
    return -1;
}

/**
 * 解析 API Key 优先级：请求体 > 数据库 > 配置文件
 * 返回可用的 api_key 指针（指向 static 缓冲区或 cJSON 值），调用者不可 free
 */
static char db_apikey_buf[256]; // 用于存储从数据库获取的 key
static const char *resolve_api_key(cJSON *apikey_item, const char *username)
{
    // 1. 请求体中的 api_key
    if (apikey_item && apikey_item->valuestring && strlen(apikey_item->valuestring) > 0) {
        return apikey_item->valuestring;
    }
    // 2. 数据库中用户的 api_key
    memset(db_apikey_buf, 0, sizeof(db_apikey_buf));
    if (username && strlen(username) > 0) {
        if (get_user_apikey_from_db(username, db_apikey_buf, sizeof(db_apikey_buf)) == 0) {
            return db_apikey_buf;
        }
    }
    // 3. 配置文件全局 key
    return dashscope_api_key;
}

/**
 * 处理 get_apikey 请求：获取用户的 API Key
 * POST body: { "user": "xxx", "token": "xxx" }
 */
static int handle_get_apikey(char *post_data)
{
    cJSON *root = cJSON_Parse(post_data);
    if (!root) {
        printf("{\"code\":1,\"msg\":\"invalid json\"}\n");
        return -1;
    }

    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *token_item = cJSON_GetObjectItem(root, "token");

    if (!user_item || !token_item) {
        printf("{\"code\":1,\"msg\":\"missing fields\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    int ret = verify_token(user_item->valuestring, token_item->valuestring);
    if (ret != 0) {
        printf("{\"code\":4,\"msg\":\"token error\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    char api_key[256] = {0};
    get_user_apikey_from_db(user_item->valuestring, api_key, sizeof(api_key));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "code", 0);
    cJSON_AddStringToObject(resp, "api_key", api_key);
    char *resp_str = cJSON_PrintUnformatted(resp);
    if (resp_str) {
        printf("%s\n", resp_str);
        free(resp_str);
    }
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return 0;
}

/**
 * 处理 set_apikey 请求：保存用户的 API Key
 * POST body: { "user": "xxx", "token": "xxx", "api_key": "sk-xxx" }
 */
static int handle_set_apikey(char *post_data)
{
    cJSON *root = cJSON_Parse(post_data);
    if (!root) {
        printf("{\"code\":1,\"msg\":\"invalid json\"}\n");
        return -1;
    }

    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *token_item = cJSON_GetObjectItem(root, "token");
    cJSON *apikey_item = cJSON_GetObjectItem(root, "api_key");

    if (!user_item || !token_item) {
        printf("{\"code\":1,\"msg\":\"missing fields\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    int ret = verify_token(user_item->valuestring, token_item->valuestring);
    if (ret != 0) {
        printf("{\"code\":4,\"msg\":\"token error\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    const char *new_key = (apikey_item && apikey_item->valuestring) ? apikey_item->valuestring : "";

    MYSQL *conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
    if (!conn) {
        printf("{\"code\":1,\"msg\":\"db error\"}\n");
        cJSON_Delete(root);
        return -1;
    }
    mysql_query(conn, "set names utf8mb4");

    char escaped_key[512] = {0};
    mysql_real_escape_string(conn, escaped_key, new_key, strlen(new_key));

    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "UPDATE user_info SET api_key='%s' WHERE user_name='%s'",
             escaped_key, user_item->valuestring);

    if (mysql_query(conn, sql) != 0) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "set_apikey failed: %s\n", mysql_error(conn));
        printf("{\"code\":1,\"msg\":\"update failed\"}\n");
        mysql_close(conn);
        cJSON_Delete(root);
        return -1;
    }

    mysql_close(conn);
    printf("{\"code\":0,\"msg\":\"ok\"}\n");
    cJSON_Delete(root);
    return 0;
}

/**
 * 处理 describe 请求：为文件生成 AI 描述 + embedding，存入 MySQL 和 FAISS
 * POST body: { "user": "xxx", "token": "xxx", "md5": "xxx", "filename": "xxx", "type": "png", "api_key": "sk-xxx" }
 */
static int handle_describe(char *post_data)
{
    int ret = -1;
    MYSQL *conn = NULL;
    cJSON *root = NULL;
    float *vector = NULL;

    root = cJSON_Parse(post_data);
    if (!root) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "parse describe request failed\n");
        printf("{\"code\":1,\"msg\":\"invalid json\"}\n");
        return -1;
    }


    // 提取参数
    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *token_item = cJSON_GetObjectItem(root, "token");
    cJSON *md5_item = cJSON_GetObjectItem(root, "md5");
    cJSON *filename_item = cJSON_GetObjectItem(root, "filename");
    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *apikey_item = cJSON_GetObjectItem(root, "api_key");
    cJSON *force_item = cJSON_GetObjectItem(root, "force");
    int force_update = (force_item && force_item->type == cJSON_True) ? 1 : 0;

    if (!user_item || !token_item || !md5_item || !filename_item) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "describe missing required fields\n");
        printf("{\"code\":1,\"msg\":\"missing fields\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    char *user = user_item->valuestring;
    char *token = token_item->valuestring;

    // 优先级：请求体 api_key > 数据库用户 key > 配置文件全局 key
    const char *api_key = resolve_api_key(apikey_item, user);
    if (!api_key || strlen(api_key) == 0) {
        printf("{\"code\":1,\"msg\":\"missing api_key\"}\n");
        cJSON_Delete(root);
        return -1;
    }
    char *md5 = md5_item->valuestring;
    char *filename = filename_item->valuestring;
    char *type = type_item ? type_item->valuestring : (char *)"";

    // 验证 token
    ret = verify_token(user, token);
    if (ret != 0) {
        printf("{\"code\":4,\"msg\":\"token error\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    // 连接数据库
    conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
    if (!conn) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "mysql connect failed\n");
        printf("{\"code\":1,\"msg\":\"db error\"}\n");
        cJSON_Delete(root);
        return -1;
    }
    mysql_query(conn, "set names utf8mb4");

    // 检查是否已有描述
    char sql_cmd[1024] = {0};
    char tmp[64] = {0};
    sprintf(sql_cmd, "select status from file_ai_desc where md5='%s'", md5);
    int qret = process_result_one(conn, sql_cmd, tmp);
    if (qret == 0 && atoi(tmp) == 1 && !force_update) {
        // 已有完成的描述且非强制更新
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "md5=%s already has description\n", md5);
        printf("{\"code\":0,\"msg\":\"already exists\"}\n");
        ret = 0;
        goto END;
    }

    // 生成文件描述
    {
        char description[4096] = {0};

        if (is_image_type(type)) {
            // 图片：调用 Qwen-VL 获取描述
            // 构造图片 URL
            char file_url[1024] = {0};

            // 从 file_info 获取 url
            memset(sql_cmd, 0, sizeof(sql_cmd));
            sprintf(sql_cmd, "select url from file_info where md5='%s'", md5);
            char db_url[512] = {0};
            if (process_result_one(conn, sql_cmd, db_url) == 0 && strlen(db_url) > 0) {
                // db_url 可能是完整 URL (http://...) 或相对路径 (group1/...)
                // 提取路径部分（从 /group 开始）
                char *path_part = strstr(db_url, "/group");
                if (!path_part) path_part = strstr(db_url, "group");

                if (strlen(public_server_ip) > 0 && path_part) {
                    // 使用公网 IP 构造 URL，让 DashScope AI 服务能访问到图片
                    snprintf(file_url, sizeof(file_url), "http://%s:%s%s%s",
                             public_server_ip, public_server_port,
                             path_part[0] == '/' ? "" : "/", path_part);
                } else if (path_part) {
                    snprintf(file_url, sizeof(file_url), "http://%s:%s%s%s",
                             web_server_ip, web_server_port,
                             path_part[0] == '/' ? "" : "/", path_part);
                } else {
                    // db_url 格式不明，直接用
                    strncpy(file_url, db_url, sizeof(file_url) - 1);
                }
            }

            if (strlen(file_url) > 0) {
                ret = dashscope_describe_image(api_key, file_url,
                                                description, sizeof(description));
                if (ret != 0) {
                    LOG(AI_LOG_MODULE, AI_LOG_PROC, "describe_image failed for %s\n", md5);
                    // 降级：使用文件名作为描述
                    snprintf(description, sizeof(description), "图片文件：%s", filename);
                }
            } else {
                snprintf(description, sizeof(description), "图片文件：%s", filename);
            }
        } else {
            // 非图片：尝试提取文件内容
            int content_extracted = 0;

            // 构造文件下载 URL（使用内网地址即可，容器间互通）
            char file_url[1024] = {0};
            memset(sql_cmd, 0, sizeof(sql_cmd));
            sprintf(sql_cmd, "select url from file_info where md5='%s'", md5);
            char db_url[512] = {0};
            if (process_result_one(conn, sql_cmd, db_url) == 0 && strlen(db_url) > 0) {
                char *path_part = strstr(db_url, "/group");
                if (!path_part) path_part = strstr(db_url, "group");
                if (path_part) {
                    snprintf(file_url, sizeof(file_url), "http://%s:%s%s%s",
                             web_server_ip, web_server_port,
                             path_part[0] == '/' ? "" : "/", path_part);
                } else {
                    strncpy(file_url, db_url, sizeof(file_url) - 1);
                }
            }

            if (strlen(file_url) > 0 && (strcasecmp(type, "docx") == 0 || is_text_type(type))) {
                // 下载文件到临时目录
                char tmp_path[512] = {0};
                snprintf(tmp_path, sizeof(tmp_path), "/tmp/ai_desc_%s.%s", md5, type);

                int dl_ret = download_file(file_url, tmp_path);
                if (dl_ret == 0) {
                    char *content_buf = (char *)malloc(8192);
                    if (content_buf) {
                        memset(content_buf, 0, 8192);

                        if (strcasecmp(type, "docx") == 0) {
                            int ext_ret = extract_docx_text(tmp_path, content_buf, 8000);
                            if (ext_ret == 0 && strlen(content_buf) > 0) {
                                content_extracted = 1;
                            }
                        } else if (is_text_type(type)) {
                            if (read_text_file(tmp_path, content_buf, 8000) == 0 && strlen(content_buf) > 0) {
                                content_extracted = 1;
                            }
                        }

                        if (content_extracted) {
                            // 截取前 3000 字符作为描述（embedding 有 token 限制）
                            int desc_len = strlen(content_buf);
                            if (desc_len > 3000) desc_len = 3000;
                            snprintf(description, sizeof(description), "文件名：%s\n文件内容：%.*s",
                                     filename, desc_len, content_buf);
                            LOG(AI_LOG_MODULE, AI_LOG_PROC, "extracted content for %s, len=%d\n", md5, desc_len);
                        }

                        free(content_buf);
                    }
                    remove(tmp_path); // 清理临时文件
                } else {
                    LOG(AI_LOG_MODULE, AI_LOG_PROC, "download file failed: %s\n", file_url);
                }
            }

            if (!content_extracted) {
                // 回退：使用文件名 + 类型
                snprintf(description, sizeof(description), "%s类型的文件：%s",
                         strlen(type) > 0 ? type : "未知", filename);
            }
        }

        LOG(AI_LOG_MODULE, AI_LOG_PROC, "description for %s: %.200s\n", md5, description);

        // 获取 embedding
        vector = (float *)malloc(sizeof(float) * embedding_dimension);
        if (!vector) {
            LOG(AI_LOG_MODULE, AI_LOG_PROC, "malloc vector failed\n");
            printf("{\"code\":1,\"msg\":\"memory error\"}\n");
            goto END;
        }
        memset(vector, 0, sizeof(float) * embedding_dimension);

        ret = dashscope_get_embedding(api_key, embedding_model,
                                       description, vector, embedding_dimension);
        if (ret != 0) {
            LOG(AI_LOG_MODULE, AI_LOG_PROC, "get_embedding failed for %s\n", md5);
            // 标记失败状态
            char escaped_desc[16384] = {0};
            mysql_real_escape_string(conn, escaped_desc, description, strlen(description));
            char *fail_sql = (char *)malloc(strlen(escaped_desc) + 1024);
            if (fail_sql) {
                sprintf(fail_sql,
                    "INSERT INTO file_ai_desc (md5, description, status) VALUES ('%s', '%s', 2) "
                    "ON DUPLICATE KEY UPDATE description='%s', status=2",
                    md5, escaped_desc, escaped_desc);
                mysql_query(conn, fail_sql);
                free(fail_sql);
            }
            printf("{\"code\":1,\"msg\":\"embedding failed\"}\n");
            goto END;
        }

        // 添加到 FAISS 索引
        int faiss_id = faiss_add(vector, embedding_dimension);
        if (faiss_id < 0) {
            LOG(AI_LOG_MODULE, AI_LOG_PROC, "faiss_add failed for %s\n", md5);
            printf("{\"code\":1,\"msg\":\"faiss error\"}\n");
            goto END;
        }

        // 存入 MySQL
        char escaped_desc[16384] = {0};
        mysql_real_escape_string(conn, escaped_desc, description, strlen(description));

        // 将 embedding 以二进制方式存储
        // 使用 hex 编码存储 embedding blob
        int blob_len = embedding_dimension * sizeof(float);
        char *hex_blob = (char *)malloc(blob_len * 2 + 1);
        if (hex_blob) {
            mysql_real_escape_string(conn, hex_blob, (char *)vector, blob_len);
        }

        // 使用 REPLACE 避免重复
        int big_sql_len = blob_len * 2 + strlen(escaped_desc) + 4096;
        char *big_sql = (char *)malloc(big_sql_len);
        if (big_sql && hex_blob) {
            sprintf(big_sql,
                "REPLACE INTO file_ai_desc (md5, description, embedding, faiss_id, model, status) "
                "VALUES ('%s', '%s', '%s', %d, '%s', 1)",
                md5, escaped_desc, hex_blob, faiss_id, embedding_model);
            if (mysql_query(conn, big_sql) != 0) {
                LOG(AI_LOG_MODULE, AI_LOG_PROC, "insert file_ai_desc failed: %s\n", mysql_error(conn));
            }
        }

        if (hex_blob) free(hex_blob);
        if (big_sql) free(big_sql);

        printf("{\"code\":0,\"msg\":\"ok\"}\n");
        ret = 0;
    }

END:
    if (vector) free(vector);
    if (conn) mysql_close(conn);
    if (root) cJSON_Delete(root);
    return ret;
}

/**
 * 处理 search 请求：语义搜索文件
 * POST body: { "user": "xxx", "token": "xxx", "query": "红色沙发上的猫", "api_key": "sk-xxx" }
 */
static int handle_search(char *post_data)
{
    int ret = -1;
    MYSQL *conn = NULL;
    cJSON *root = NULL;
    float *query_vec = NULL;

    root = cJSON_Parse(post_data);
    if (!root) {
        printf("{\"code\":1,\"msg\":\"invalid json\"}\n");
        return -1;
    }

    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *token_item = cJSON_GetObjectItem(root, "token");
    cJSON *query_item = cJSON_GetObjectItem(root, "query");
    cJSON *apikey_item = cJSON_GetObjectItem(root, "api_key");

    if (!user_item || !token_item || !query_item) {
        printf("{\"code\":1,\"msg\":\"missing fields\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    char *user = user_item->valuestring;
    char *token = token_item->valuestring;
    char *query = query_item->valuestring;

    // 优先级：请求体 api_key > 数据库用户 key > 配置文件全局 key
    const char *api_key = resolve_api_key(apikey_item, user);
    if (!api_key || strlen(api_key) == 0) {
        printf("{\"code\":1,\"msg\":\"missing api_key\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    // 验证 token
    ret = verify_token(user, token);
    if (ret != 0) {
        printf("{\"code\":4,\"msg\":\"token error\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    if (strlen(query) == 0) {
        printf("{\"code\":1,\"msg\":\"empty query\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    // 检查索引是否为空
    if (faiss_get_ntotal() == 0) {
        printf("{\"code\":0,\"count\":0,\"files\":[]}\n");
        cJSON_Delete(root);
        return 0;
    }

    // 获取查询文本的 embedding
    query_vec = (float *)malloc(sizeof(float) * embedding_dimension);
    if (!query_vec) {
        printf("{\"code\":1,\"msg\":\"memory error\"}\n");
        cJSON_Delete(root);
        return -1;
    }
    memset(query_vec, 0, sizeof(float) * embedding_dimension);

    ret = dashscope_get_embedding(api_key, embedding_model,
                                   query, query_vec, embedding_dimension);
    if (ret != 0) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "search embedding failed\n");
        printf("{\"code\":1,\"msg\":\"embedding failed\"}\n");
        goto END;
    }

    // L2 归一化查询向量（使内积 = 余弦相似度）
    vector_l2_normalize(query_vec, embedding_dimension);

    // FAISS 搜索
    {
        int topk = 10;
        long ids[10] = {0};
        float scores[10] = {0};
        // 余弦相似度阈值：低于此值的结果认为不相关
        float score_threshold = 0.45f;

        int found = faiss_search(query_vec, embedding_dimension, topk, ids, scores);
        if (found <= 0) {
            printf("{\"code\":0,\"count\":0,\"files\":[]}\n");
            ret = 0;
            goto END;
        }

        // 连接数据库查询文件信息
        conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
        if (!conn) {
            printf("{\"code\":1,\"msg\":\"db error\"}\n");
            goto END;
        }
        mysql_query(conn, "set names utf8mb4");

        // 构造返回 JSON
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "code", 0);
        cJSON *files_arr = cJSON_CreateArray();

        for (int i = 0; i < found; i++) {
            // 过滤掉低相似度结果
            if (scores[i] < score_threshold) continue;

            // 通过 faiss_id 查 file_ai_desc，再 join file_info
            char sql_cmd[1024] = {0};
            sprintf(sql_cmd,
                "SELECT fad.md5, fad.description, fi.url, fi.size, fi.type "
                "FROM file_ai_desc fad "
                "JOIN file_info fi ON fad.md5 = fi.md5 "
                "WHERE fad.faiss_id = %ld",
                ids[i]);

            MYSQL_RES *res = NULL;
            if (mysql_query(conn, sql_cmd) != 0) {
                LOG(AI_LOG_MODULE, AI_LOG_PROC, "search query failed: %s\n", mysql_error(conn));
                continue;
            }
            res = mysql_store_result(conn);
            if (!res) continue;

            MYSQL_ROW row = mysql_fetch_row(res);
            if (row) {
                // 查找该用户的文件名
                char file_md5[256] = {0};
                strncpy(file_md5, row[0] ? row[0] : "", sizeof(file_md5) - 1);

                char fname_sql[1024] = {0};
                sprintf(fname_sql,
                    "SELECT file_name FROM user_file_list WHERE user='%s' AND md5='%s' LIMIT 1",
                    user, file_md5);

                char file_name[256] = {0};
                // 先查用户自己的文件
                int fname_ret = process_result_one(conn, fname_sql, file_name);
                if (fname_ret != 0) {
                    // 尝试查所有用户的文件名
                    sprintf(fname_sql,
                        "SELECT file_name FROM user_file_list WHERE md5='%s' LIMIT 1",
                        file_md5);
                    process_result_one(conn, fname_sql, file_name);
                }

                cJSON *file_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(file_obj, "md5", file_md5);
                cJSON_AddStringToObject(file_obj, "filename", file_name);
                cJSON_AddStringToObject(file_obj, "description", row[1] ? row[1] : "");

                // 文件 URL（数据库已存储完整 URL）
                if (row[2] && strlen(row[2]) > 0) {
                    cJSON_AddStringToObject(file_obj, "url", row[2]);
                } else {
                    cJSON_AddStringToObject(file_obj, "url", "");
                }

                cJSON_AddStringToObject(file_obj, "size", row[3] ? row[3] : "0");
                cJSON_AddStringToObject(file_obj, "type", row[4] ? row[4] : "");
                cJSON_AddNumberToObject(file_obj, "score", scores[i]);
                cJSON_AddItemToArray(files_arr, file_obj);
            }
            mysql_free_result(res);
        }

        cJSON_AddNumberToObject(resp, "count", cJSON_GetArraySize(files_arr));
        cJSON_AddItemToObject(resp, "files", files_arr);

        char *resp_str = cJSON_PrintUnformatted(resp);
        if (resp_str) {
            printf("%s\n", resp_str);
            free(resp_str);
        }
        cJSON_Delete(resp);
        ret = 0;
    }

END:
    if (query_vec) free(query_vec);
    if (conn) mysql_close(conn);
    if (root) cJSON_Delete(root);
    return ret;
}

/**
 * 处理 rebuild 请求：从 MySQL 重建 FAISS 索引
 * POST body: { "user": "xxx", "token": "xxx" }
 */
static int handle_rebuild(char *post_data)
{
    int ret = -1;
    MYSQL *conn = NULL;
    cJSON *root = NULL;

    root = cJSON_Parse(post_data);
    if (!root) {
        printf("{\"code\":1,\"msg\":\"invalid json\"}\n");
        return -1;
    }

    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *token_item = cJSON_GetObjectItem(root, "token");

    if (!user_item || !token_item) {
        printf("{\"code\":1,\"msg\":\"missing fields\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    ret = verify_token(user_item->valuestring, token_item->valuestring);
    if (ret != 0) {
        printf("{\"code\":4,\"msg\":\"token error\"}\n");
        cJSON_Delete(root);
        return -1;
    }

    conn = msql_conn(mysql_user, mysql_pwd, mysql_db);
    if (!conn) {
        printf("{\"code\":1,\"msg\":\"db error\"}\n");
        cJSON_Delete(root);
        return -1;
    }
    mysql_query(conn, "set names utf8mb4");

    // 重新初始化空索引
    faiss_reset();
    remove(faiss_index_path);
    faiss_init(faiss_index_path, embedding_dimension);

    // 查询所有已完成的记录
    char sql_cmd[512] = {0};
    sprintf(sql_cmd, "SELECT id, md5, embedding FROM file_ai_desc WHERE status=1 AND embedding IS NOT NULL ORDER BY id");

    if (mysql_query(conn, sql_cmd) != 0) {
        LOG(AI_LOG_MODULE, AI_LOG_PROC, "rebuild query failed: %s\n", mysql_error(conn));
        printf("{\"code\":1,\"msg\":\"query failed\"}\n");
        mysql_close(conn);
        cJSON_Delete(root);
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        printf("{\"code\":1,\"msg\":\"no result\"}\n");
        mysql_close(conn);
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    MYSQL_ROW row;
    unsigned long *lengths;
    while ((row = mysql_fetch_row(res))) {
        lengths = mysql_fetch_lengths(res);
        if (!row[2] || lengths[2] == 0) continue;

        int expected_size = embedding_dimension * (int)sizeof(float);
        if ((int)lengths[2] != expected_size) {
            LOG(AI_LOG_MODULE, AI_LOG_PROC, "rebuild: embedding size mismatch for id=%s\n", row[0]);
            continue;
        }

        float *vec = (float *)row[2];
        int faiss_id = faiss_add(vec, embedding_dimension);
        if (faiss_id >= 0) {
            // 更新 faiss_id
            char update_sql[512] = {0};
            sprintf(update_sql, "UPDATE file_ai_desc SET faiss_id=%d WHERE id=%s", faiss_id, row[0]);
            mysql_query(conn, update_sql);
            count++;
        }
    }
    mysql_free_result(res);

    LOG(AI_LOG_MODULE, AI_LOG_PROC, "rebuild done: %d vectors added\n", count);
    printf("{\"code\":0,\"msg\":\"rebuilt\",\"count\":%d}\n", count);

    mysql_close(conn);
    cJSON_Delete(root);
    return 0;
}

int main()
{
    // 读取配置
    read_cfg();

    // 初始化 libcurl 全局
    curl_global_init(CURL_GLOBAL_ALL);

    // 创建 FAISS 索引目录
    {
        // 从 index_path 中提取目录
        char dir[512] = {0};
        strncpy(dir, faiss_index_path, sizeof(dir) - 1);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            char mkdir_cmd[1024] = {0};
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir);
            system(mkdir_cmd);
        }
    }

    // 初始化 FAISS 索引
    faiss_init(faiss_index_path, embedding_dimension);

    LOG(AI_LOG_MODULE, AI_LOG_PROC, "ai_cgi started, faiss ntotal=%ld\n", faiss_get_ntotal());

    // FastCGI 主循环
    while (FCGI_Accept() >= 0) {
        char *query_string = getenv("QUERY_STRING");
        char *content_length_str = getenv("CONTENT_LENGTH");

        // 输出 HTTP 头
        printf("Content-Type: application/json\r\n\r\n");

        // 获取 cmd 参数
        char cmd[64] = {0};
        if (query_string) {
            int cmd_len = 0;
            query_parse_key_value(query_string, "cmd", cmd, &cmd_len);
        }

        if (strlen(cmd) == 0) {
            printf("{\"code\":1,\"msg\":\"missing cmd\"}\n");
            continue;
        }

        // 读取 POST body
        char *post_data = NULL;
        int content_length = 0;

        if (content_length_str) {
            content_length = atoi(content_length_str);
        }

        if (content_length > 0 && content_length < 1024 * 1024) {
            post_data = (char *)malloc(content_length + 1);
            if (post_data) {
                int bytes_read = fread(post_data, 1, content_length, stdin);
                post_data[bytes_read] = '\0';
            }
        }

        if (!post_data || strlen(post_data) == 0) {
            printf("{\"code\":1,\"msg\":\"no post data\"}\n");
            if (post_data) free(post_data);
            continue;
        }

        LOG(AI_LOG_MODULE, AI_LOG_PROC, "cmd=%s, data=%.200s\n", cmd, post_data);

        // 分发处理
        if (strcmp(cmd, "describe") == 0) {
            handle_describe(post_data);
        } else if (strcmp(cmd, "search") == 0) {
            handle_search(post_data);
        } else if (strcmp(cmd, "rebuild") == 0) {
            handle_rebuild(post_data);
        } else if (strcmp(cmd, "get_apikey") == 0) {
            handle_get_apikey(post_data);
        } else if (strcmp(cmd, "set_apikey") == 0) {
            handle_set_apikey(post_data);
        } else {
            printf("{\"code\":1,\"msg\":\"unknown cmd: %s\"}\n", cmd);
        }

        if (post_data) free(post_data);
    }

    curl_global_cleanup();
    return 0;
}
